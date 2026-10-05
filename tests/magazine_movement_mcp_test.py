"""Saved Move/Rotate paths must drive the physical magazine and survive export."""
import json,sys,uuid,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit,simulation
from server import command

def rotate(q,v):
    x,y,z,w=q;a,b,c=v
    t=[2*(y*c-z*b),2*(z*a-x*c),2*(x*b-y*a)]
    return [a+w*t[0]+y*t[2]-z*t[1],b+w*t[1]+z*t[0]-x*t[2],c+w*t[2]+x*t[1]-y*t[0]]

def run():
    work=ROOT/'local/magazine-movement-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=ready()['project'];original_bytes=Path(original).read_bytes();created=[];identity=str(uuid.uuid4())
    try:
        edit('testbed',dict(name='glock'));ready();project=Path(ready()['project']);saved=project.read_bytes()
        try:
            edit('workspace',dict(index=5));ready()
            reference=command('magazine_reference_state')['settings'];reference['connectionMode']='custom';reference['connectionInCarrier']['translate'][0]+=.75
            edit('magazine_reference',dict(operation='save',settings=reference));ready()
            before=simulation();state=command('magazine_setup_state');settings=state['settings'];rail=before['magazineInteraction']
            recipe=command('reload');track=recipe['tracks'][state['fullTrack']];q=track['poses'][0][3:7]
            direction=rotate([-q[0],-q[1],-q[2],q[3]],[v*rail['travel']*3 for v in rail['outwardInWeapon']])
            stage=lambda axis,value,kind='translation':dict(forward='Extract',reverse='Insert',motion=kind,axis=axis,**{'from':0,'to':value},pivot=[0,0,0],release='hold',returnRate=10)
            stages=[stage(i,v) for i,v in enumerate(direction) if abs(v)>.0001]+[stage(0,20,'rotation')]
            movement=dict(format='reload-maker-part-mechanism',version=1,convention='staged-part-local-v1',id=identity,
                name='Temporary magazine integration movement',weaponRecord=recipe['source']['weaponRecord'],revision=0,anchor=track['anchor'],
                partInWeaponAtRest=dict(rotate=[1,0,0,0,1,0,0,0,1],translate=[0,0,0],scale=1),followers=[],stages=stages)
            file=work/'movement.json';file.write_text(json.dumps(movement),encoding='utf-8');created=edit('import_records',dict(paths=[str(file)]))['files']
            assert identity in [m['id'] for m in command('magazine_setup_state')['movements']]
            edit('magazine_setup',dict(operation='save',settings=dict(settings,motion='manual',movementId=identity)));ready()
            after=simulation();a,b=before['magazineInteraction'],after['magazineInteraction']
            assert a['outwardInWeapon']==b['outwardInWeapon'] and a['travel']==b['travel']
            assert len(b['motionSamples'])>16
            captured=json.loads(Path(edit('export',dict(kind='runtime_configuration'))['path']).read_text(encoding='utf-8'))
            assert captured['magazine']['motionSource']['definition']['id']==identity
            assert len(captured['magazine']['capturePointInMagazine'])==3
            assert len(captured['magazine']['motion'])==len(b['motionSamples'])
            entries=json.loads((ROOT/'workspace/packs/verified-b5bf9bf910a249da8f180b951aa17ae1/projects.json').read_text(encoding='utf-8'))
            source=Path(next(row['path'] for row in entries if row['name']=='Glock'));owner=json.loads(source.read_text(encoding='utf-8'))['weapons'][0]
            docs={key:json.loads((source.parent/owner[key]['path']).read_text(encoding='utf-8')) for key in ('profiles','magazines','sounds','grabs')}
            selection=captured['selection'];profiles=docs['profiles']['profiles']
            matches=[i for i,p in enumerate(profiles) if all(p.get(k,docs['profiles'].get('standardReceiver') if k=='receiver' else None).lower()==v.lower() for k,v in selection.items())]
            assert len(matches)==1;profiles[matches[0]]=captured['profile']
            magazines=docs['magazines']['magazines'];index=next(i for i,m in enumerate(magazines) if m['record'].lower()==selection['magazine'].lower());magazines[index]=captured['magazine']
            for key,value in docs.items():(work/(key+'.json')).write_text(json.dumps(value),encoding='utf-8')
            check=subprocess.run([str(ROOT/'build-fast/dist/ReloadMakerV2/backends/diamond/DiamondProfileCheck.exe'),'--authored',*[str(work/(k+'.json')) for k in docs]],capture_output=True,text=True)
            assert check.returncode==0,(check.stdout,check.stderr)
            path=work/'manual.project.json';edit('save_project',dict(path=str(path)));edit('open_project',dict(path=str(path)));ready()
            assert simulation()==after
            edit('simulate',dict(operation='mag_out'));edit('simulate',dict(operation='step',seconds=1));render=command('render',dict(view='iso',fit=True))
            (work/'result.json').write_text(json.dumps(dict(ok=True,render=render,motion=captured['magazine']),indent=2),encoding='utf-8')
            edit('magazine_setup',dict(operation='reset'));ready();assert simulation()==before
            print('PASS authored translation/rotation, independent straight guide, frozen runtime path and reopen:',work,flush=True)
        finally:
            project.write_bytes(saved);command('open_project',dict(path=str(project)),command('revision')['revision']);ready();edit('save_project',dict(path=str(project)));project.write_bytes(saved)
    finally:
        Path(original).write_bytes(original_bytes);command('open_project',dict(path=original),command('revision')['revision']);ready();edit('save_project',dict(path=original));Path(original).write_bytes(original_bytes)
        for file in created:
            path=Path(file).resolve();assert path.parent==(ROOT/'workspace/part-mechanisms').resolve() and path.stem==identity;path.unlink()

if __name__=='__main__':run()
