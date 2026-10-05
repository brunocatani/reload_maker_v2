"""A saved staged bolt cycle must drive opening, closing and ejection."""
import json
from pathlib import Path
import sys
import uuid
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit
from server import command

def simulation():return json.loads(Path(edit('export',dict(kind='simulation'))['path']).read_text())
def run():
    work=ROOT/'local/operating-bolt-tests'/uuid.uuid4().hex;work.mkdir(parents=True);created=[]
    original=ready()['project'];original_bytes=Path(original).read_bytes()
    try:
        edit('testbed',dict(name='timberwolf'));ready();project=Path(ready()['project']);saved=project.read_bytes()
        try:
            edit('workspace',dict(index=5));ready();before=simulation();settings=command('operating_setup_state')['settings'];recipe=command('reload')
            index=settings['operating'];identity=str(uuid.uuid4())
            def stage(name,rotation,value):return dict(forward=name,reverse='Reverse '+name,motion='rotation' if rotation else 'translation',axis=1,
                                                      **{'from':0,'to':value},pivot=[0,0,0],release='hold',returnRate=10)
            movement=dict(format='reload-maker-part-mechanism',version=1,convention='staged-part-local-v1',id=identity,
                name='Temporary complete bolt integration cycle',weaponRecord=recipe['source']['weaponRecord'],revision=0,anchor=recipe['tracks'][index]['anchor'],
                partInWeaponAtRest=dict(rotate=[1,0,0,0,1,0,0,0,1],translate=[0,0,0],scale=1),followers=[],
                stages=[stage('Lift',True,60),stage('Open',False,-8),stage('Close',False,8),stage('Lock',True,-60)])
            path=work/'movement.json';path.write_text(json.dumps(movement));created=edit('import_records',dict(paths=[str(path)]))['files']
            settings.update(mode='manual',followers=[index],movements=[dict(track=index,id=identity)],duration=2,ejectSeconds=-1)
            edit('operating_setup',dict(operation='save',settings=settings));ready()
            state=command('simulation_state');assert state['active'] and not state['error'],state
            current=simulation();cycle=current['boltMotion'];assert 7.9<cycle['inputTravel']<8.1 and cycle['ejectProgress']==1
            assert cycle['openingProgress'][0]==0 and cycle['openingProgress'][-1]==1
            assert cycle['closingProgress'][0]==0 and cycle['closingProgress'][-1]==1
            assert cycle['source']['method']=='authored-part-local-stages'
            edit('operating_setup',dict(operation='reset'));ready();assert simulation()==before
            (work/'result.json').write_text(json.dumps(dict(ok=True,inputTravel=cycle['inputTravel'],ejectProgress=cycle['ejectProgress']),indent=2))
            print('PASS authored lift/open/close/lock cycle, full-opening ejection and original Timberwolf restoration:',work,flush=True)
        finally:
            project.write_bytes(saved);command('open_project',dict(path=str(project)),command('revision')['revision']);ready()
            edit('save_project',dict(path=str(project)));project.write_bytes(saved)
    finally:
        Path(original).write_bytes(original_bytes);command('open_project',dict(path=original),command('revision')['revision']);ready()
        edit('save_project',dict(path=original));Path(original).write_bytes(original_bytes)
        for file in created:
            path=Path(file).resolve();assert path.parent==(ROOT/'workspace/part-mechanisms').resolve() and path.stem==identity;path.unlink()

if __name__=='__main__':run()
