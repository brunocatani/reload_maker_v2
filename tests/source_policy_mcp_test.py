"""Known admission rules survive new captures, explicit editing and project reopening."""
import json,sys,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit,simulation
from server import command
from runtime_capture_mcp_test import NAMES

def run():
    work=ROOT/'local/source-policy-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=ready()['project'];original_bytes=Path(original).read_bytes();results=[]
    source=Path('F:/fo4dev/PROJECT_ROCK_V2/main_projects/DIAMOND/tests/fixtures/testbeds')
    projects=json.loads((ROOT/'workspace/packs/verified-b5bf9bf910a249da8f180b951aa17ae1/projects.json').read_text(encoding='utf-8'))
    try:
        for name,file in [('mk18','MK18'),('mk18-empire','MK18Empire'),('thanatos','Thanatos'),('srep-service','SREPService')]:
            edit('testbed',dict(name=name));ready();project=Path(ready()['project']);saved=project.read_bytes()
            try:
                folder=work/name;folder.mkdir();fixture=json.loads(saved)
                origin=Path(next(row['path'] for row in projects if row['name']==NAMES[name]))
                owner=json.loads(origin.read_text(encoding='utf-8'))['weapons'][0]
                for point in json.loads((origin.parent/owner['grabs']['path']).read_text(encoding='utf-8'))['locations']:
                    identity='|'.join(point[k] for k in ('weapon','group','sourceModel','sourcePath')).replace('\\','/').lower()
                    fixture.setdefault('grabLocations',{}).setdefault(identity,point)
                (folder/'fixture.project.json').write_text(json.dumps(fixture),encoding='utf-8')
                edit('open_project',dict(path=str(folder/'fixture.project.json')));ready()
                edit('workspace',dict(index=5));ready();before=simulation()
                reference=json.loads((source/(file+'.json')).read_text(encoding='utf-8'))
                expected={k:reference[k] for k in ('sourceOverride','blockedMods','magazineCollections') if k in reference}
                state=command('source_policy_state');assert state['policy']==expected,(name,state['policy'],expected)
                edit('source_policy',dict(operation='save',key=state['key'],policy=expected));ready();assert simulation()==before
                capture=json.loads(Path(edit('export',dict(kind='runtime_configuration'))['path']).read_text(encoding='utf-8'))
                assert capture['sourcePolicyEdited'] and capture['sourcePolicy']==expected
                assert all(capture['profileHeader'][key]==value for key,value in expected.items())
                (folder/'capture.json').write_text(json.dumps(capture,indent=2),encoding='utf-8')
                path=work/(name+'.project.json');edit('save_project',dict(path=str(path)));edit('open_project',dict(path=str(path)));ready()
                assert command('source_policy_state')['policy']==expected and simulation()==before
                try:edit('source_policy',dict(operation='save',key='wrong-context',policy={}))
                except RuntimeError:pass
                else:raise AssertionError('Policy from another source context accepted')
                edit('source_policy',dict(operation='reset',key=state['key']));ready();assert command('source_policy_state')['policy']==expected
                results.append(dict(testbed=name,policy=expected));print('PASS',name,flush=True)
            finally:
                project.write_bytes(saved);command('open_project',dict(path=str(project)),command('state')['revision']);ready();edit('save_project',dict(path=str(project)));project.write_bytes(saved)
    finally:
        Path(original).write_bytes(original_bytes);edit('open_project',dict(path=original));ready();edit('save_project',dict(path=original));Path(original).write_bytes(original_bytes)
    (work/'result.json').write_text(json.dumps(dict(ok=True,results=results),indent=2),encoding='utf-8');print('Evidence:',work,flush=True)

if __name__=='__main__':run()
