"""Atomically save one live configuration into a shareable pack project.

GUI, MCP and command-line callers use this operation. Existing configurations
are replaced by their native selector identities; other authored work is retained.
"""
import copy
import os
from pathlib import Path
import shutil
import subprocess
import uuid

from product_export import form_key, inspect_project, project_source
from product_packs import TEMPLATES, cancellation, digest, identity, read, require, write


def native_validate(documents, folder, backends):
    validator=backends.application/'backends/diamond/DiamondProfileCheck.exe'
    require(validator.is_file(),'The bundled DIAMOND validator is missing')
    paths=[]
    for kind in ('profiles','magazines','sounds','grabs'):
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


def save_configuration(project_path,capture_path,backends,expected='',pack_id='',full='',empty=''):
    path,capture_path=Path(project_path).resolve(),Path(capture_path).resolve()
    snapshot=read(capture_path);snapshot_hash=digest(capture_path)
    require(snapshot.get('format')=='reload-maker-runtime-configuration' and snapshot.get('version')==1,
            'Select a live runtime configuration captured by Maker')
    require(snapshot.get('worldModelSpace')=='magazine-carrier','The capture must use the native magazine carrier frame')
    record=form_key(snapshot.get('weapon'));template=snapshot.get('template');require(template in TEMPLATES,'Unsupported behavior template')
    magazine=form_key(snapshot['selection']['magazine'])
    existing=path.exists()
    if existing:
        require(expected and digest(path)==expected,'Pack project changed; inspect it again before saving')
        project,_=inspect_project(path);project=copy.deepcopy(project)
    else:
        require(not expected,'The selected project is missing')
        pack_id=identity(pack_id);require('/' not in pack_id,'Choose a single pack namespace')
        project=dict(format='reload-maker-pack-project',version=1,id=pack_id,name=snapshot.get('name',pack_id),revision=0,
                     requirements={},weapons=[],objects=[])
    weapon=next((item for item in project['weapons'] if form_key(item['record'])==record),None)
    if weapon:
        require(weapon['template']==template,'This weapon already uses a different template in this pack')
        documents={kind:read(project_source(path,weapon[kind])) for kind in ('profiles','magazines','sounds','grabs')}
    else:
        weapon=dict(record=record,name=snapshot.get('name',record),template=template,magazineObjects=[]);project['weapons'].append(weapon)
        require(isinstance(snapshot.get('profileHeader'),dict),'Capture the current native selector catalog again')
        documents=dict(profiles={**copy.deepcopy(snapshot['profileHeader']),'profiles':[]},
                       magazines=dict(format='diamond-magazine-profiles',version=3,weapon=record,magazines=[]),
                       sounds=dict(format='diamond-sound-pack',version=1,profiles=[]),
                       grabs=dict(format='diamond-grab-locations',version=1,locations=[]))
    # Existing source override, collection, blocked-conversion and attachment
    # rules remain authoritative. Capturing one configuration cannot erase them.
    profiles=documents['profiles'];default=profiles.get('standardReceiver','')
    selector=lambda p:tuple(form_key(p.get(k,default) or default) if (p.get(k) or (k=='receiver' and default)) else ''
                            for k in ('receiver','magazine','operatingPart'))
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
            logical=project['id']+'/magazine/'+magazine.replace(':','/')
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
            require(all(current_resources.get(r['path'].replace('\\','/').lower())==r['sha256'] for r in source['sources']['resources']),
                    'World model source bytes differ from the configured reload')
            asset_inputs.append((model,receipt,digest(model),digest(receipt)))
            item=dict(id=assignment[presentation],name=f"{weapon['name']} {snapshot['magazine']['capacity']} round magazine ({presentation})",
                      bounds=meta['bounds'],weight=meta['massKg'],value=0,source=dict(path=str(model),sha256=meta['outputSha256']))
            replace_record(project['objects'],item,lambda p:p['id'])
    capture_records=weapon.setdefault('captures',[])
    replace_record(capture_records,dict(selection=snapshot['selection'],sources=snapshot['sources']),lambda p:selector(p['selection']))
    path.parent.mkdir(parents=True,exist_ok=True)
    stage=path.parent/('.authoring-'+uuid.uuid4().hex);stage.mkdir()
    final=path.parent/'authoring'/uuid.uuid4().hex
    try:
        cancellation();files=native_validate(documents,stage,backends)
        for kind,file in zip(('profiles','magazines','sounds','grabs'),files):
            weapon[kind]=dict(path=str((final/file.name).relative_to(path.parent)).replace('\\','/'),sha256=digest(file))
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
        collect(documents);project['requirements']['plugins']=sorted(plugins,key=str.lower)
        project['revision']+=1
        probe=copy.deepcopy(project)
        for item in probe['weapons']:
            for kind in ('profiles','magazines','sounds','grabs'):
                item[kind]['path']=str(stage/(kind+'.json')) if form_key(item['record'])==record else str(project_source(path,item[kind]))
        for item in probe['objects']:
            source=Path(item['source']['path']);item['source']['path']=str(source if source.is_absolute() else path.parent/source)
        write(stage/'project-check.json',probe);inspect_project(stage/'project-check.json');(stage/'project-check.json').unlink()
        require(digest(capture_path)==snapshot_hash,'Live capture changed during save')
        for model,receipt,model_hash,receipt_hash in asset_inputs:
            require(digest(model)==model_hash and digest(receipt)==receipt_hash,'Finished model changed during save')
        require(digest(path)==expected if existing else not path.exists(),'Pack project changed during save')
        cancellation();final.parent.mkdir(parents=True,exist_ok=True);os.replace(stage,final)
        # Commit the pointer last. Until this one atomic replacement, every
        # previous behavior document/model remains the active project content.
        write(path,project)
        return dict(ok=True,project=str(path),sha256=digest(path),revision=project['revision'],weapon=record,
                    variants=len(profiles['profiles']),magazines=len(documents['magazines']['magazines']),
                    objects=len(project['objects']),updatedSelection=snapshot['selection'])
    finally:
        if stage.exists() and stage.parent==path.parent and stage.name.startswith('.authoring-'):shutil.rmtree(stage)
