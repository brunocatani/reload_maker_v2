"""Atomically save live configurations into a shareable pack project.

GUI, MCP and command-line callers use this operation. Existing configurations
are replaced by their native selector identities; other authored work is retained.
Saving one configuration and saving all of a weapon's configurations run the
same code: each capture is applied and checked by DIAMOND on its own, and the
project is written once at the end.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid

from product_export import form_key, inspect_project, project_source
from product_packs import TEMPLATES, cancellation, digest, identity, read, require, write

KINDS = ('profiles', 'magazines', 'sounds', 'grabs')


def native_validate(documents, folder, backends):
    validator=backends.application/'backends/diamond/DiamondProfileCheck.exe'
    require(validator.is_file(),'The bundled DIAMOND validator is missing')
    paths=[]
    for kind in KINDS:
        path=folder/(kind+'.json');write(path,documents[kind]);paths.append(path)
    checked=subprocess.run([str(validator),'--authored',*[('\\\\?\\'+str(p)) if os.name=='nt' else str(p) for p in paths]],
                           capture_output=True,text=True,timeout=60,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    require(checked.returncode==0,'DIAMOND rejected the configured reload: '+(checked.stderr or checked.stdout).strip())
    return paths


def replace_record(items,value,key):
    matches=[i for i,item in enumerate(items) if key(item)==key(value)]
    require(len(matches)<=1,'The project contains duplicate authored identities')
    if matches:items[matches[0]]=copy.deepcopy(value)
    else:items.append(copy.deepcopy(value))

def policy_identity(policy):
    require(isinstance(policy,dict) and set(policy)<= {'sourceOverride','blockedMods','magazineCollections'},'Invalid source compatibility rules')
    marker=None
    if 'sourceOverride' in policy:
        value=policy['sourceOverride']
        require(isinstance(value,dict) and set(value)=={'marker','present'} and type(value['present']) is bool,'Invalid add-on marker condition')
        marker=(form_key(value['marker']),value['present'])
    groups=[];seen=set()
    for field in ('blockedMods','magazineCollections'):
        values=policy.get(field,[])
        require(isinstance(values,list) and len(values)<=32,'Too many source compatibility records')
        records=[form_key(value) for value in values]
        require(len(set(records))==len(records) and not seen.intersection(records),'One source record has duplicate or conflicting compatibility roles')
        seen.update(records);groups.append(tuple(sorted(records)))
    return marker,*groups


def input_hash(snapshot):
    """Everything a configuration is saved from: the capture holds the source
    checksums, the authored setup and the hand fits it uses."""
    return hashlib.sha256(json.dumps(snapshot,sort_keys=True,separators=(',',':'),allow_nan=False).encode()).hexdigest()


def load_capture(capture_path):
    path=Path(capture_path).resolve()
    snapshot=read(path)
    require(snapshot.get('format')=='reload-maker-runtime-configuration' and snapshot.get('version')==1,
            'Select a live runtime configuration captured by Maker')
    require(snapshot.get('worldModelSpace')=='magazine-carrier','The capture must use the native magazine carrier frame')
    return dict(path=path,snapshot=snapshot,sha256=digest(path),inputHash=input_hash(snapshot))


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

    def saved_hash(self,snapshot):
        """The input hash this project saved for the capture's configuration, if any."""
        record=form_key(snapshot.get('weapon'))
        weapon=next((item for item in self.project['weapons'] if form_key(item['record'])==record),None)
        if not weapon:
            return None
        if record not in self.documents:
            self.documents[record]={kind:read(project_source(self.path,weapon[kind])) for kind in KINDS}
        selector=selector_for(self.documents[record]['profiles'])
        saved=next((c for c in weapon.get('captures',[]) if selector(c['selection'])==selector(snapshot['selection'])),None)
        return saved.get('inputHash') if saved else None


def apply_capture(draft,capture,full='',empty=''):
    """Writes one capture into the draft. Returns the finished models it uses."""
    snapshot=capture['snapshot']
    record,weapon,documents=draft.weapon(snapshot)
    magazine=form_key(snapshot['selection']['magazine'])
    project=draft.project
    # Defaults must not silently mix two source editions. An explicit editor
    # compatibility edit updates the shared weapon policy; source snapshots and
    # the complete native parser still validate every remaining configuration.
    profiles=documents['profiles']
    if 'sourcePolicy' in snapshot:
        policy=snapshot['sourcePolicy'];fields=('sourceOverride','blockedMods','magazineCollections')
        incoming_policy=policy_identity(policy)
        existing_policy={key:profiles[key] for key in fields if key in profiles}
        if snapshot.get('sourcePolicyEdited') is True:
            for key in fields:profiles.pop(key,None)
            profiles.update(copy.deepcopy(policy))
        else:
            require(policy_identity(existing_policy)==incoming_policy,'The pack uses another source compatibility policy. Choose its source-context pack or explicitly review and save the weapon-wide compatibility rules.')
    selector=selector_for(profiles)
    replace_record(profiles['profiles'],snapshot['profile'],selector)
    for incoming in snapshot.get('profileHeader',{}).get('slots',[]):
        slot=next((s for s in profiles['slots'] if s['name']==incoming['name']),None)
        if slot is None:profiles['slots'].append(copy.deepcopy(incoming))
        else:
            keys={form_key(k) for k in slot['records']}
            for key in incoming['records']:
                if form_key(key) not in keys:slot['records'].append(key);keys.add(form_key(key))
    if snapshot.get('profileHeader',{}).get('embeddedMagazineReceivers'):
        records=profiles.setdefault('embeddedMagazineReceivers',[])
        for key in snapshot['profileHeader']['embeddedMagazineReceivers']:
            if form_key(key) not in {form_key(k) for k in records}:records.append(key)
    prior_magazine=next((m for m in documents['magazines']['magazines'] if form_key(m['record'])==magazine),None)
    if prior_magazine and prior_magazine.get('presentationSetup')!=snapshot['magazine'].get('presentationSetup'):
        require(full and empty,'Magazine presentation changed; recapture and cook both world models before saving')
    replace_record(documents['magazines']['magazines'],snapshot['magazine'],lambda p:form_key(p['record']))
    replace_record(documents['sounds']['profiles'],snapshot['sounds'],lambda p:(form_key(p['weapon']),form_key(p['magazine'])))
    if snapshot['grabs']:
        replace_record(documents['grabs']['locations'],snapshot['grabs'],lambda p:tuple(p[k].replace('\\','/').lower()
                       for k in ('weapon','group','sourceModel','sourcePath')))
    assignment=next((a for a in weapon['magazineObjects'] if form_key(a['record'])==magazine),None)
    require(bool(full)==bool(empty),'Choose both a full and an empty finished model')
    require(assignment is not None or full,'A new magazine needs its full and empty cooked models')
    asset_inputs=[]
    if full:
        if assignment is None:
            # Two weapons may use the same source OMOD but need different hand
            # fits/carrier frames. Their runtime MISC identities stay distinct;
            # the publisher can still deduplicate identical finished NIF bytes.
            logical=project['id']+'/magazine/'+hashlib.sha256((record+'|'+magazine).encode()).hexdigest()[:24]
            assignment=dict(record=magazine,full=logical+'/full',empty=logical+'/empty');weapon['magazineObjects'].append(assignment)
        current_resources={r['path'].replace('\\','/').lower():r['sha256'] for r in snapshot['sources']['resources']}
        for presentation,model in (('full',full),('empty',empty)):
            model=Path(model).resolve();receipt=model.with_suffix('.receipt.json');meta=read(receipt)
            require(meta.get('engineCooked') and digest(model)==meta.get('outputSha256'),'Finished model differs from its cook receipt')
            provenance=meta.get('capture',{});source=provenance.get('referencePose',{})
            require(source.get('space')=='magazine-carrier' and source.get('presentation')==presentation and
                    form_key(provenance.get('weapon'))==record and form_key(source.get('magazineRecord'))==magazine,
                    'Finished model belongs to another weapon, magazine, presentation or reference frame')
            require(source.get('sourceRecipeId')==snapshot['magazine'].get('sourceRecipeId'),
                    'Magazine motion changed; recapture both world models for this configured reload')
            require(source.get('presentationSetup')==snapshot['magazine'].get('presentationSetup'),
                    'Magazine model was cooked with another body/payload presentation')
            require(all(current_resources.get(r['path'].replace('\\','/').lower())==r['sha256'] for r in source['sources']['resources']),
                    'World model source bytes differ from the configured reload')
            asset_inputs.append((model,receipt,digest(model),digest(receipt)))
            item=dict(id=assignment[presentation],name=f"{weapon['name']} {snapshot['magazine']['capacity']} round magazine ({presentation})",
                      bounds=meta['bounds'],weight=meta['massKg'],value=0,source=dict(path=str(model),sha256=meta['outputSha256']))
            replace_record(project['objects'],item,lambda p:p['id'])
    capture_records=weapon.setdefault('captures',[])
    require(isinstance(snapshot.get('runtimeSources'),list) and snapshot['runtimeSources'],'Recapture this configuration with its runtime source requirements')
    runtime_sources=dict(version=1,weapon=snapshot['weapon'],resources=snapshot['runtimeSources'])
    replace_record(capture_records,dict(selection=snapshot['selection'],sources=runtime_sources,inputHash=capture['inputHash']),
                   lambda p:selector(p['selection']))
    return asset_inputs


def checked(draft,record,backends):
    """DIAMOND's verdict on one weapon's documents in the draft."""
    with tempfile.TemporaryDirectory(prefix='.authoring-check-',dir=draft.path.parent) as folder:
        native_validate(draft.documents[record],Path(folder),backends)


def save(project_path,captures,backends,expected='',pack_id='',full='',empty='',skip_unchanged=False,strict=True):
    """Applies the captures in order and writes the project once.

    With strict, any failure fails the save. Otherwise each failing capture is
    reported and left out while the others are saved; with skip_unchanged a
    configuration saved from identical inputs is reported up to date."""
    path=Path(project_path).resolve()
    require(captures,'Select at least one captured configuration')
    loaded=[load_capture(c) for c in captures]
    existing=path.exists()
    if existing:
        require(expected and digest(path)==expected,'Pack project changed; inspect it again before saving')
        project,_=inspect_project(path);project=copy.deepcopy(project)
    else:
        require(not expected,'The selected project is missing')
        pack_id=identity(pack_id);require('/' not in pack_id,'Choose a single pack namespace')
        project=dict(format='reload-maker-pack-project',version=1,id=pack_id,name=loaded[0]['snapshot'].get('name',pack_id),revision=0,
                     requirements={},weapons=[],objects=[])
    path.parent.mkdir(parents=True,exist_ok=True)
    draft=Draft(path,project)
    rows,asset_inputs,touched=[],[],set()
    for capture in loaded:
        cancellation()
        snapshot=capture['snapshot'];row=dict(selection=snapshot.get('selection',{}),capture=str(capture['path']))
        if skip_unchanged and existing and draft.saved_hash(snapshot)==capture['inputHash']:
            rows.append(dict(row,status='up-to-date'));continue
        trial=Draft(path,copy.deepcopy(draft.project));trial.documents=copy.deepcopy(draft.documents)
        try:
            models=apply_capture(trial,capture,full,empty)
            record=form_key(snapshot.get('weapon'))
            if not strict:
                checked(trial,record,backends)
        except ValueError as error:
            if strict:
                raise
            rows.append(dict(row,status='failed',error=str(error)));continue
        draft=trial;asset_inputs+=models;touched.add(record)
        rows.append(dict(row,status='saved'))
    saved=[r for r in rows if r['status']=='saved']
    summary=dict(ok=True,project=str(path),rows=rows,saved=len(saved),upToDate=sum(r['status']=='up-to-date' for r in rows),
                 failed=sum(r['status']=='failed' for r in rows))
    if not saved:
        return dict(summary,sha256=digest(path) if existing else '',revision=project['revision'])
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
                weapon[kind]=dict(path=str((final/file.relative_to(stage)).relative_to(path.parent)).replace('\\','/'),sha256=digest(file))
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
        probe=copy.deepcopy(project)
        for item in probe['weapons']:
            for kind in KINDS:
                record=form_key(item['record'])
                item[kind]['path']=str(folders[record]/(kind+'.json')) if record in touched else str(project_source(path,item[kind]))
        for item in probe['objects']:
            source=Path(item['source']['path']);item['source']['path']=str(source if source.is_absolute() else path.parent/source)
        write(stage/'project-check.json',probe);inspect_project(stage/'project-check.json');(stage/'project-check.json').unlink()
        for capture in loaded:
            require(digest(capture['path'])==capture['sha256'],'Live capture changed during save')
        for model,receipt,model_hash,receipt_hash in asset_inputs:
            require(digest(model)==model_hash and digest(receipt)==receipt_hash,'Finished model changed during save')
        require(digest(path)==expected if existing else not path.exists(),'Pack project changed during save')
        cancellation();final.parent.mkdir(parents=True,exist_ok=True);os.replace(stage,final)
        # Commit the pointer last. Until this one atomic replacement, every
        # previous behavior document/model remains the active project content.
        write(path,project)
        last=form_key(loaded[-1]['snapshot']['weapon']) if len(loaded)==1 else None
        result=dict(summary,sha256=digest(path),revision=project['revision'],objects=len(project['objects']))
        if last:
            result.update(weapon=last,variants=len(draft.documents[last]['profiles']['profiles']),
                          magazines=len(draft.documents[last]['magazines']['magazines']),updatedSelection=loaded[0]['snapshot']['selection'])
        return result
    finally:
        if stage.exists() and stage.parent==path.parent and stage.name.startswith('.authoring-'):shutil.rmtree(stage)


def save_configuration(project_path,capture_path,backends,expected='',pack_id='',full='',empty=''):
    """Saves one configuration; any failure fails the save."""
    return save(project_path,[capture_path],backends,expected,pack_id,full,empty)


def save_configurations(project_path,capture_paths,backends,expected='',pack_id=''):
    """Saves every configuration that passes; unchanged ones are skipped and
    failing ones reported, none of them blocking the rest."""
    return save(project_path,capture_paths,backends,expected,pack_id,skip_unchanged=True,strict=False)
