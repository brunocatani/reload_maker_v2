"""One weapon's automatic native setup keeps its full simulation when saved as times."""
import argparse
import json
from pathlib import Path
import sys
import time
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
sys.path.insert(0,str(ROOT/'tests'))
from editor_session import EditorRun,command,edit,ready
import product_library as library
from operating_setup_mcp_test import authored_loadout
from source_time_window_mcp_test import close

def run(weapon,explicit=False):
    plugin=weapon.split(':')[0];folder=ROOT/'workspace/library/plugins'/plugin
    snapshot=library.read_snapshot(folder,library.read_project(folder)['latest'])
    entry=snapshot['weapons'][weapon]
    output=ROOT/'local/native-time-compat-tests'/str(time.time_ns());output.mkdir(parents=True)
    with EditorRun():
        edit('open_source',dict(path=snapshot['source']['package']));state=ready()
        selected=next(w for w in state['weapons'] if (w['plugin'].lower()==plugin and int(w['formId'],16)&0xffffff==int(weapon.split(':')[1],16)) or w.get('editorId','').lower()==entry.get('editorId','!').lower())
        edit('select_weapon',dict(index=selected['index']));ready()
        project=output/'original.project.json';edit('export',dict(kind='project',path=str(project)))
        authored_loadout(project)
        edit('workspace',dict(index=5));ready()
        settings=command('operating_setup_state')['settings']
        assert settings['version']==2 and settings['openSeconds']==settings['closedSeconds']==-1,settings
        def simulation(name):
            value=json.loads(Path(edit('export',dict(kind='simulation'))['path']).read_text())
            (output/(name+'.json')).write_text(json.dumps(value,indent=2))
            for key in ('operatingSetup','authoredCurves','motionSource'):value.pop(key,None)
            return value
        before=simulation('before')
        edit('operating_setup',dict(operation='save',settings=settings));ready()
        close(before,simulation('after'),'simulation')
        edited=output/'time.project.json';edit('export',dict(kind='project',path=str(edited)))
        edit('import_project',dict(path=str(edited)));ready()
        close(before,simulation('reopened'),'reopened simulation')
        if explicit:
            state=command('operating_setup_state');settings=state['settings']
            baseline=json.loads(Path(edit('export',dict(kind='runtime_configuration'))['path']).read_text())
            settings.update(openSeconds=state['suggestedOpenSeconds'],closedSeconds=state['suggestedClosedSeconds'])
            edit('operating_setup',dict(operation='save',settings=settings));ready()
            current=json.loads(Path(edit('export',dict(kind='runtime_configuration'))['path']).read_text())
            (output/'explicit-runtime.json').write_text(json.dumps(current,indent=2))
            assert abs(current['profile']['duration']-baseline['profile']['duration'])<1e-5
            assert len(current['profile']['tracks'])==len(baseline['profile']['tracks'])
            for old,new in zip(baseline['profile']['tracks'],current['profile']['tracks']):
                assert old['name']==new['name'] and old['sourcePath']==new['sourcePath']
                close(old['poses'][0],new['poses'][0],old['name']+'/open')
                close(old['poses'][-1],new['poses'][-1],old['name']+'/closed')
            for field in ('capacity','grip','rightGrip','parts','handInMagazine','travel','seat','outward','guide'):
                if field in baseline['magazine']:close(baseline['magazine'][field],current['magazine'][field],'magazine/'+field)
            close(baseline['sounds'],current['sounds'],'sounds')
            print('PASS',weapon,'explicit source window, follower endpoints, magazine data and sounds',flush=True)
        print('PASS',weapon,'native automatic setup, full simulation and reopen; evidence',output,flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('weapon');parser.add_argument('--explicit',action='store_true');args=parser.parse_args();run(args.weapon.lower(),args.explicit)
