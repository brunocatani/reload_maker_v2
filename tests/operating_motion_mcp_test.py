"""Author an actual movement, apply/export it, and restore the accepted source."""
import json
from pathlib import Path
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit
from server import command

def export(kind):return json.loads(Path(edit('export',dict(kind=kind))['path']).read_text())
def run():
    work=ROOT/'local/operating-motion-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=command('export',{'kind':'project'},ready()['revision'])['path'];original_bytes=Path(original).read_bytes();created=[]
    try:
        edit('testbed',{'name':'glock'});ready();project=Path(command('export',{'kind':'project'},ready()['revision'])['path']);saved=project.read_bytes()
        try:
            edit('workspace',{'index':5});ready();before=export('slide');state=command('operating_setup_state');settings=state['settings']
            native=dict(settings,mode='clip')
            edit('operating_setup',dict(operation='save',settings=native));ready();sampled=export('slide')
            for a,b in zip(before['tracks'],sampled['tracks']):
                assert a['anchor']==b['anchor'] and len(a['poses'])==len(b['poses'])
                assert max(abs(x-y) for p,q in zip(a['poses'],b['poses']) for x,y in zip(p,q))<.002
            recipe=command('reload');index=settings['operating'];anchor=recipe['tracks'][index]['anchor'];identity=str(uuid.uuid4())
            movement=dict(format='reload-maker-part-mechanism',version=1,convention='staged-part-local-v1',id=identity,
                name='Temporary operating integration movement',weaponRecord=recipe['source']['weaponRecord'],revision=0,anchor=anchor,
                partInWeaponAtRest=dict(rotate=[1,0,0,0,1,0,0,0,1],translate=[0,0,0],scale=1),followers=[],
                stages=[dict(forward='Retract',reverse='Return',motion='translation',axis=1,**{'from':0,'to':-2},pivot=[0,0,0],release='rest',returnRate=10)])
            file=work/'movement.json';file.write_text(json.dumps(movement));created=edit('import_records',dict(paths=[str(file)]))['files']
            manual=dict(settings,mode='manual',followers=[index],movements=[dict(track=index,id=identity)])
            edit('operating_setup',dict(operation='save',settings=manual));ready();assert command('simulation_state')['active']
            result=export('runtime_configuration');track=result['profile']['tracks'][0]['poses']
            assert 1.98<sum((a-b)**2 for a,b in zip(track[0][:3],track[-1][:3]))**.5<2.02
            assert result['profile']['motionSource']['method']=='authored-part-local-stages'
            assert result['profile']['motionSource']['definitions'][0]['id']==identity
            edit('export',dict(kind='project',path=str(work/'manual.project.json')))
            edit('import_project',dict(path=str(work/'manual.project.json')));ready()
            assert export('runtime_configuration')['profile']['tracks']==result['profile']['tracks']
            edit('operating_setup',dict(operation='reset'));ready();assert export('slide')==before
            (work/'result.json').write_text(json.dumps(dict(ok=True,nativeClipPreserved=True,manualMovementExported=True,reopen=True),indent=2))
            print('PASS alternate native clip, real manual movement, runtime export/reopen and original behavior restoration:',work,flush=True)
        finally:
            project.write_bytes(saved);command('import_project',dict(path=str(project)),command('revision')['revision']);ready()
            edit('export',dict(kind='project',path=str(project)));project.write_bytes(saved)
    finally:
        Path(original).write_bytes(original_bytes);command('import_project',dict(path=original),command('revision')['revision']);ready()
        edit('export',dict(kind='project',path=original));Path(original).write_bytes(original_bytes)
        for file in created:
            path=Path(file).resolve();assert path.parent==(ROOT/'workspace/part-mechanisms').resolve() and path.stem==identity
            path.unlink()

if __name__=='__main__':run()
