"""Explicit magazine frames preserve native fits and persist independent connections."""
import argparse,copy,json,sys,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit,simulation
from server import command

def close(a,b,path=''):
    if isinstance(a,(float,int)) and not isinstance(a,bool):assert abs(a-b)<.002,(path,a,b)
    elif isinstance(a,list):
        assert len(a)==len(b),(path,len(a),len(b))
        for i,(x,y) in enumerate(zip(a,b)):close(x,y,path+'/'+str(i))
    elif isinstance(a,dict):
        for k,v in a.items():
            if k not in ('revision','documentRevision','session'):close(v,b[k],path+'/'+k)
    else:assert a==b,(path,a,b)

def run(names):
    work=ROOT/'local/magazine-reference-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=command('export',{'kind':'project'},ready()['revision'])['path'];original_bytes=Path(original).read_bytes();results=[]
    try:
        for name in names:
            edit('testbed',dict(name=name));ready();project=Path(command('export',{'kind':'project'},ready()['revision'])['path']);saved=project.read_bytes()
            try:
                edit('workspace',dict(index=5));ready();before=simulation();state=command('magazine_reference_state');settings=state['settings']
                assert settings['anchorKey'] in [a['key'] for a in state['anchors']],state
                edit('magazine_reference',dict(operation='save',settings=settings));ready();close(before,simulation())
                edit('simulate',dict(operation='drop_magazine'));edit('simulate',dict(operation='grab_magazine'))
                pose=command('pose');edit('magazine_reference',dict(operation='reset'));ready()
                edit('simulate',dict(operation='drop_magazine'));edit('simulate',dict(operation='grab_magazine'));close(pose,command('pose'))
                edit('simulate',dict(operation='reset'))
                custom=copy.deepcopy(settings);custom['connectionMode']='custom';custom['connectionInCarrier']['translate'][0]+=.75
                edit('magazine_reference',dict(operation='save',settings=custom));ready();after=simulation()
                a,b=before['magazineInteraction'],after['magazineInteraction']
                assert abs(a['connection']['translate'][0]-b['connection']['translate'][0])>.1
                for key in ('motionSamples','outwardInWeapon','travel'):
                    if key in a:close(a[key],b[key],key)
                path=work/(name+'.project.json');edit('export',dict(kind='project',path=str(path)));edit('import_project',dict(path=str(path)));ready();close(after,simulation())
                bad=copy.deepcopy(custom);bad['anchorKey']='0'*64
                try:edit('magazine_reference',dict(operation='save',settings=bad))
                except RuntimeError:pass
                else:raise AssertionError('Missing frame accepted')
                edit('magazine_reference',dict(operation='reset'));ready();close(before,simulation())
                results.append(dict(testbed=name,fitPreserved=True,connectionReopen=True));print('PASS',name,flush=True)
            finally:
                project.write_bytes(saved);command('import_project',dict(path=str(project)),command('revision')['revision']);ready();edit('export',dict(kind='project',path=str(project)));project.write_bytes(saved)
        (work/'result.json').write_text(json.dumps(dict(ok=True,results=results),indent=2),encoding='utf-8');print('Evidence:',work,flush=True)
    except Exception:
        (work/'failure.json').write_text(json.dumps(dict(state=command('state'),reference=command('magazine_reference_state')),indent=2),encoding='utf-8');raise
    finally:
        Path(original).write_bytes(original_bytes);command('import_project',dict(path=original),command('revision')['revision']);ready();edit('export',dict(kind='project',path=original));Path(original).write_bytes(original_bytes)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--testbed',action='append');run(p.parse_args().testbed or ['glock','ru556','m1a','timberwolf','srep-service'])
