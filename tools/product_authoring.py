"""Atomically save live configurations into a shareable pack project.

GUI, MCP and command-line callers use this operation. Existing configurations
are replaced by their native selector identities; other authored work is retained.
Saving one configuration and saving all of a weapon's configurations run the
same code: each capture is applied and checked by DIAMOND on its own, and the
project is written once at the end.
"""
import copy
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid

from product_export import form_key, inspect_project, project_source
from product_packs import TEMPLATES, cancellation, identity, read, require, write

KINDS = ('profiles', 'magazines', 'sounds', 'grabs')


class Rejected(ValueError):
    """DIAMOND rejected a weapon's documents."""


def native_validate(documents, folder, backends):
    validator=backends.application/'backends/diamond/DiamondProfileCheck.exe'
    require(validator.is_file(),'The bundled DIAMOND validator is missing')
    paths=[]
    for kind in KINDS:
        path=folder/(kind+'.json');write(path,documents[kind]);paths.append(path)
    checked=subprocess.run([str(validator),'--authored',*[('\\\\?\\'+str(p)) if os.name=='nt' else str(p) for p in paths]],
                           capture_output=True,text=True,timeout=60,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if checked.returncode!=0:
        raise Rejected('DIAMOND rejected the configured reload: '+(checked.stderr or checked.stdout).strip())
    return paths


def replace_record(items,value,key):
    matches=[i for i,item in enumerate(items) if key(item)==key(value)]
    require(len(matches)<=1,'The project contains duplicate authored identities')
    if matches:items[matches[0]]=copy.deepcopy(value)
    else:items.append(copy.deepcopy(value))


def merge_configuration_slots(existing,incoming):
    """Combine display labels without duplicating the source records they own.

    Separate native attach points may share a label (1911 has two Material
    slots). The runtime catalog groups by label, not by those attach points.
    Normalize both sides so the initial capture and subsequent saves agree.
    """
    slots={}
    records={}
    for source in (*existing,*incoming):
        name=source['name']
        slot=slots.setdefault(name,dict(name=name,records=[]))
        seen=records.setdefault(name,set())
        for record in source['records']:
            key=form_key(record)
            if key not in seen:
                slot['records'].append(record)
                seen.add(key)
    return list(slots.values())

def policy_identity(policy):
    require(isinstance(policy,dict) and set(policy)<= {'sourceOverride','blockedMods','magazineCollections','weaponAliases'},'Invalid source compatibility rules')
    marker=None
    if 'sourceOverride' in policy:
        value=policy['sourceOverride']
        require(isinstance(value,dict) and set(value)=={'marker','present'} and type(value['present']) is bool,'Invalid add-on marker condition')
        marker=(form_key(value['marker']),value['present'])
    groups=[];seen=set()
    for field in ('blockedMods','magazineCollections','weaponAliases'):
        values=policy.get(field,[])
        require(isinstance(values,list) and len(values)<=32,'Too many source compatibility records')
        records=[form_key(value) for value in values]
        require(len(set(records))==len(records) and not seen.intersection(records),'One source record has duplicate or conflicting compatibility roles')
        seen.update(records);groups.append(tuple(sorted(records)))
    return marker,*groups


def presentation_identity(setup):
    """What a magazine's cooked models depend on in its presentation setup.
    Motion and capture bookkeeping do not change the cooked body/payload.
    Missing older metadata is unknown, not evidence of different geometry."""
    if isinstance(setup,dict):
        ignored={'recipeId','trackAnchors','motion','movementId','entrySeconds'}
        if not setup.get('sampledPayload'):
            ignored.update(('loadedFrame','emptyFrame'))
        return {key:value for key,value in setup.items() if key not in ignored}
    return setup


def load_capture(capture_path):
    path=Path(capture_path).resolve()
    snapshot=read(path)
    require(snapshot.get('format')=='reload-maker-runtime-configuration' and snapshot.get('version')==1,
            'Select a live runtime configuration captured by Maker')
    require(snapshot.get('worldModelSpace')=='magazine-carrier','The capture must use the native magazine carrier frame')
    return dict(path=path,snapshot=snapshot)


def selector_for(profiles):
    default=profiles.get('standardReceiver','')
    return lambda p:tuple(form_key(p.get(k,default) or default) if (p.get(k) or (k=='receiver' and default)) else ''
                          for k in ('receiver','magazine','operatingPart'))


class Draft:
    """The project and the behavior documents of each weapon it touches."""

    def __init__(self,path,project):
        self.path=path
        self.project=project
        self.documents={}

    def weapon(self,snapshot):
        record=form_key(snapshot.get('weapon'));template=snapshot.get('template')
        require(template in TEMPLATES,'Unsupported behavior template')
        weapon=next((item for item in self.project['weapons'] if form_key(item['record'])==record),None)
        if weapon:
            require(weapon['template']==template,'This weapon already uses a different template in this pack')
            if record not in self.documents:
                self.documents[record]={kind:read(project_source(self.path,weapon[kind])) for kind in KINDS}
        else:
            require(isinstance(snapshot.get('profileHeader'),dict),'Capture the current native selector catalog again')
            weapon=dict(record=record,name=snapshot.get('name',record),template=template,magazineObjects=[]);self.project['weapons'].append(weapon)
            self.documents[record]=dict(profiles={**copy.deepcopy(snapshot['profileHeader']),'profiles':[]},
                                        magazines=dict(format='diamond-magazine-profiles',version=3,weapon=record,magazines=[]),
                                        sounds=dict(format='diamond-sound-pack',version=1,profiles=[]),
                                        grabs=dict(format='diamond-grab-locations',version=1,locations=[]))
        return record,weapon,self.documents[record]

def apply_capture(draft,capture,full='',empty=''):
    """Writes one capture into the draft."""
    snapshot=capture['snapshot']
    record,weapon,documents=draft.weapon(snapshot)
    magazine=form_key(snapshot['selection']['magazine'])
    project=draft.project
    # Defaults must not silently mix two source editions. An explicit editor
    # compatibility edit updates the shared weapon policy; source snapshots and
    # the complete native parser still validate every remaining configuration.
    profiles=documents['profiles']
    # The weapon-level header follows the current capture (format, version,
    # weapon, standard receiver, strictness, magazine attach point). A document
    # saved under a retired format (the Glock's reload-maker-glock-slide-pack,
    # 2026-10-05) keeps its variants and reads as what DIAMOND validates today;
    # slots and embedded receivers merge below, the policy fields are separate.
    for key,value in snapshot.get('profileHeader',{}).items():
        if key not in ('slots','embeddedMagazineReceivers','profiles'):profiles[key]=copy.deepcopy(value)
    if 'sourcePolicy' in snapshot:
        policy=snapshot['sourcePolicy'];fields=('sourceOverride','blockedMods','magazineCollections','weaponAliases')
        incoming_policy=policy_identity(policy)
        existing_policy={key:profiles[key] for key in fields if key in profiles}
        if snapshot.get('sourcePolicyEdited') is True:
            for key in fields:profiles.pop(key,None)
            profiles.update(copy.deepcopy(policy))
        else:
            require(policy_identity(existing_policy)==incoming_policy,'The pack uses another source compatibility policy. Choose its source-context pack or explicitly review and save the weapon-wide compatibility rules.')
    selector=selector_for(profiles)
    replace_record(profiles['profiles'],snapshot['profile'],selector)
    profiles['slots']=merge_configuration_slots(profiles['slots'],snapshot.get('profileHeader',{}).get('slots',[]))
    if snapshot.get('profileHeader',{}).get('embeddedMagazineReceivers'):
        records=profiles.setdefault('embeddedMagazineReceivers',[])
        for key in snapshot['profileHeader']['embeddedMagazineReceivers']:
            if form_key(key) not in {form_key(k) for k in records}:records.append(key)
    prior_magazine=next((m for m in documents['magazines']['magazines'] if form_key(m['record'])==magazine),None)
    before,after=presentation_identity(prior_magazine.get('presentationSetup')) if prior_magazine else None,presentation_identity(snapshot['magazine'].get('presentationSetup'))
    if prior_magazine and before is not None and after is not None and before!=after:
        changed=sorted(k for k in set(before or {})|set(after or {}) if (before or {}).get(k)!=(after or {}).get(k)) if isinstance(before,dict) and isinstance(after,dict) else ['setup']
        require(full and empty,'Magazine presentation changed ('+', '.join(changed)+'); recapture and cook both world models before saving')
    replace_record(documents['magazines']['magazines'],snapshot['magazine'],lambda p:form_key(p['record']))
    replace_record(documents['sounds']['profiles'],snapshot['sounds'],lambda p:(form_key(p['weapon']),form_key(p['magazine'])))
    if snapshot['grabs']:
        replace_record(documents['grabs']['locations'],snapshot['grabs'],lambda p:tuple(p[k].replace('\\','/').lower()
                       for k in ('weapon','group','sourceModel','sourcePath')))
    assignment=next((a for a in weapon['magazineObjects'] if form_key(a['record'])==magazine),None)
    require(bool(full)==bool(empty),'Choose both a full and an empty finished model')
    require(assignment is not None or full,'A new magazine needs its full and empty cooked models')
    if full:
        if assignment is None:
            # Two weapons may use the same source OMOD but need different hand
            # fits/carrier frames. Their runtime MISC identities stay distinct;
            # the publisher can still deduplicate identical finished NIF bytes.
            logical=project['id']+'/magazine/'+hashlib.sha256((record+'|'+magazine).encode()).hexdigest()[:24]
            assignment=dict(record=magazine,full=logical+'/full',empty=logical+'/empty');weapon['magazineObjects'].append(assignment)
        for presentation,model in (('full',full),('empty',empty)):
            model=Path(model).resolve();receipt=model.with_suffix('.receipt.json');meta=read(receipt)
            require(meta.get('engineCooked'),'Choose a finished model cooked by Maker')
            provenance=meta.get('capture',{});source=provenance.get('referencePose',{})
            # Stat-only receiver records can contain the same physical cell.
            # Its cooked carrier-space body is reusable across those records;
            # presentation ownership below still distinguishes different pieces.
            source_model=str(source.get('model','')).replace('\\','/').casefold()
            current_model=str(snapshot['magazine'].get('model','')).replace('\\','/').casefold()
            same_source=form_key(source.get('magazineRecord'))==magazine or bool(source_model and source_model==current_model)
            require(source.get('space')=='magazine-carrier' and source.get('presentation')==presentation and same_source,
                    'Finished model belongs to another magazine model, presentation or reference frame')
            cooked=presentation_identity(source.get('presentationSetup'))
            current=presentation_identity(snapshot['magazine'].get('presentationSetup'))
            require(cooked is None or current is None or cooked==current,
                    'Magazine model was cooked with another body/payload presentation')
            item=dict(id=assignment[presentation],name=(f"{weapon['name']} shell" if snapshot['magazine'].get('unitLoading') else f"{weapon['name']} {snapshot['magazine']['capacity']} round magazine ({presentation})"),
                      bounds=meta['bounds'],weight=meta['massKg'],value=0,source=dict(path=str(model)))
            replace_record(project['objects'],item,lambda p:p['id'])
    capture_records=weapon.setdefault('captures',[])
    replace_record(capture_records,dict(selection=snapshot['selection']),
                   lambda p:selector(p['selection']))


def checked(draft,record,backends):
    """DIAMOND's verdict on one weapon's documents in the draft."""
    with tempfile.TemporaryDirectory(prefix='.authoring-check-',dir=draft.path.parent) as folder:
        native_validate(draft.documents[record],Path(folder),backends)


def save(project_path,captures,backends,pack_id='',full='',empty='',strict=True,models=None):
    """Applies the captures in order and writes the project once.

    DIAMOND checks each saved weapon once. Only when it rejects one, and the
    save is not strict, is the save repeated checking every capture on its
    own, so the rejected ones are reported and the rest are kept."""
    try:
        return _save(project_path,captures,backends,pack_id,full,empty,strict,models,isolate=False)
    except Rejected:
        if strict:
            raise
        return _save(project_path,captures,backends,pack_id,full,empty,strict,models,isolate=True)


def _save(project_path,captures,backends,pack_id,full,empty,strict,models,isolate):
    """One pass of save(); with isolate, DIAMOND checks every capture.

    `models` maps a lower-case magazine record to its (full, empty) finished
    models; a capture of that magazine uses them instead of `full`/`empty`.

    With strict, any failure fails the save. Otherwise each failing capture is
    reported and left out while the others are saved. A template change must
    replace every existing configuration together; it cannot relabel old
    behavior under a new family."""
    path=Path(project_path).resolve()
    require(captures,'Select at least one captured configuration')
    loaded=[load_capture(c) for c in captures]
    existing=path.exists()
    if existing:
        project,_=inspect_project(path);project=copy.deepcopy(project)
    else:
        pack_id=identity(pack_id);require('/' not in pack_id,'Choose a single pack namespace')
        project=dict(format='reload-maker-pack-project',version=1,id=pack_id,name=loaded[0]['snapshot'].get('name',pack_id),revision=0,
                     requirements={},weapons=[],objects=[])
    path.parent.mkdir(parents=True,exist_ok=True)
    draft=Draft(path,project)
    changed_templates={}
    for weapon in project['weapons']:
        record=form_key(weapon['record'])
        incoming=[c['snapshot'] for c in loaded if form_key(c['snapshot']['weapon'])==record]
        templates={s['template'] for s in incoming}
        if not templates or templates=={weapon['template']}:
            continue
        require(len(templates)==1 and next(iter(templates)) in TEMPLATES,'One weapon must use one supported template')
        profiles=read(project_source(path,weapon['profiles']))
        selector=selector_for(profiles)
        previous={selector(p) for p in profiles['profiles']}
        require(previous <= {selector(s['selection']) for s in incoming},
                'The behavior template changed. Save all configurations of this weapon to replace its previous behavior together.')
        weapon['template']=next(iter(templates));changed_templates[record]=(selector,previous)
    rows,touched=[],set()
    for capture in loaded:
        cancellation()
        snapshot=capture['snapshot'];row=dict(selection=snapshot.get('selection',{}),capture=str(capture['path']))
        trial=Draft(path,copy.deepcopy(draft.project));trial.documents=copy.deepcopy(draft.documents)
        try:
            magazine=form_key(snapshot.get('selection',{}).get('magazine',''))
            apply_capture(trial,capture,*((models or {}).get(magazine) or (full,empty)))
            record=form_key(snapshot.get('weapon'))
            if isolate:
                checked(trial,record,backends)
        except ValueError as error:
            changed=changed_templates.get(form_key(snapshot['weapon']))
            replacing=changed and changed[0](snapshot['selection']) in changed[1]
            if strict or replacing:
                raise
            rows.append(dict(row,status='failed',error=str(error)));continue
        draft=trial;touched.add(record)
        rows.append(dict(row,status='saved'))
    saved=[r for r in rows if r['status']=='saved']
    summary=dict(ok=bool(saved),project=str(path),rows=rows,saved=len(saved),upToDate=0,
                 failed=sum(r['status']=='failed' for r in rows))
    if not saved:
        summary['error']='No configuration was saved. '+next((r['error'] for r in rows if r['status']=='failed'),'No valid captures.')
        return dict(summary,revision=project['revision'])
    stage=path.parent/('.authoring-'+uuid.uuid4().hex);stage.mkdir()
    final=path.parent/'authoring'/uuid.uuid4().hex
    try:
        cancellation()
        # One weapon's documents sit directly in the generation folder; several
        # weapons each get their own folder in it.
        folders={record:(stage if len(touched)==1 else stage/record.replace(':','-')) for record in touched}
        for record in sorted(touched):
            weapon=next(item for item in draft.project['weapons'] if form_key(item['record'])==record)
            folder=folders[record];folder.mkdir(exist_ok=True)
            files=native_validate(draft.documents[record],folder,backends)
            for kind,file in zip(KINDS,files):
                weapon[kind]=dict(path=str((final/file.relative_to(stage)).relative_to(path.parent)).replace('\\','/'))
        project=draft.project
        plugins=set(project.setdefault('requirements',{}).get('plugins',[]))
        def collect(value):
            if isinstance(value,str):
                try:plugins.add(form_key(value).split(':')[0])
                except ValueError:pass
            elif isinstance(value,list):
                for item in value:collect(item)
            elif isinstance(value,dict):
                for key,item in value.items():
                    if key!='sourceOverride':collect(item)
        for record in touched:collect(draft.documents[record])
        project['requirements']['plugins']=sorted(plugins,key=str.lower)
        project['revision']+=1
        cancellation();final.parent.mkdir(parents=True,exist_ok=True);os.replace(stage,final)
        # Commit the pointer last. Until this one atomic replacement, every
        # previous behavior document/model remains the active project content.
        write(path,project)
        last=form_key(loaded[-1]['snapshot']['weapon']) if len(loaded)==1 else None
        result=dict(summary,revision=project['revision'],objects=len(project['objects']))
        if last:
            result.update(weapon=last,variants=len(draft.documents[last]['profiles']['profiles']),
                          magazines=len(draft.documents[last]['magazines']['magazines']),updatedSelection=loaded[0]['snapshot']['selection'])
        return result
    finally:
        if stage.exists() and stage.parent==path.parent and stage.name.startswith('.authoring-'):shutil.rmtree(stage)


def save_configuration(project_path,capture_path,backends,pack_id='',full='',empty=''):
    """Saves one configuration; any failure fails the save."""
    return save(project_path,[capture_path],backends,pack_id,full,empty)


def save_configurations(project_path,capture_paths,backends,pack_id=''):
    """Save current captures by selector; a changed template is atomic."""
    return save(project_path,capture_paths,backends,pack_id,strict=False)
