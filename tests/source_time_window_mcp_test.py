"""1911 native source windows: all slides/magazines, exact poses and save round trips.

Run once per weapon, through the editor. --save keeps the deliberate final
library save; without it the library is never written. Source packages stay read-only.
"""
import argparse
import json
from pathlib import Path
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from editor_session import EditorRun,command,edit,ready
import product_library as library

def close(a,b,path=''):
    if isinstance(a,(int,float)) and not isinstance(a,bool) and isinstance(b,(int,float)):
        assert abs(a-b)<.001,(path,a,b)
    elif isinstance(a,dict):
        assert a.keys()==b.keys(),(path,a.keys(),b.keys())
        for k in a:close(a[k],b[k],path+'/'+k)
    elif isinstance(a,list):
        assert len(a)==len(b),(path,len(a),len(b))
        for i,(x,y) in enumerate(zip(a,b)):close(x,y,path+'/'+str(i))
    else:assert a==b,(path,a,b)

def select(record):
    state=ready()
    slot,choice=next((s,c) for s in state['slots'] for c in s['choices'] if c['record'].lower()==record)
    edit('configure',dict(slot=slot['key'],mod=choice['id']));ready()

def capture(folder,name):
    edit('workspace',dict(index=5));ready()
    state=command('simulation_state');assert state['active'],state
    result=edit('export',dict(kind='runtime_configuration'))
    value=json.loads(Path(result['path']).read_text())
    (folder/(name+'.json')).write_text(json.dumps(value,indent=2))
    return value

def run(save):
    folder=ROOT/'local/source-time-window-tests'/str(time.time_ns());folder.mkdir(parents=True)
    plugin='eb_1911.esp';weapon=plugin+':000808'
    root=ROOT/'workspace/library/plugins'/plugin
    snapshot=library.read_snapshot(root,library.read_project(root)['latest'])
    source=snapshot['source']['package']
    slides=['003da9','003daa','003dc6'];magazines=['00081f','000825','004d41','0092ab','009a49']
    with EditorRun(keep_library=save):
        edit('open_source',dict(path=source));state=ready()
        pick=next(w for w in state['weapons'] if w['plugin'].lower()==plugin and int(w['formId'],16)&0xffffff==0x808)
        edit('select_weapon',dict(index=pick['index']));ready()
        rows=[]
        for slide in slides:
            select(plugin+':'+slide)
            before={}
            for magazine in magazines:
                select(plugin+':'+magazine)
                before[magazine]=capture(folder,slide+'-'+magazine+'-before')
            select(plugin+':00081f')
            edit('workspace',dict(index=5));ready()
            settings=command('operating_setup_state')['settings']
            assert settings['mode']=='clip' and settings['version']==2,settings
            assert abs(settings['openSeconds']-.15)<1e-6 and settings['closedSeconds']==0,settings
            edit('operating_setup',dict(operation='save',settings=settings));ready()
            control=before['00081f']['profile']
            for magazine in magazines:
                select(plugin+':'+magazine)
                value=capture(folder,slide+'-'+magazine+'-after')
                assert abs(value['profile']['duration']-.15)<1e-6,value['profile']['duration']
                assert value['profile']['sourceFrames']==control['sourceFrames']
                close(control['tracks'],value['profile']['tracks'],'operating tracks')
                # Compare physical magazine data, excluding capture identity/provenance.
                for field in ('capacity','grip','rightGrip','parts','handInMagazine','travel','seat','outward','guide'):
                    if field in before[magazine]['magazine']:
                        close(before[magazine]['magazine'][field],value['magazine'][field],'magazine/'+field)
                close(before[magazine]['sounds'],value['sounds'],'sounds')
                current=command('operating_setup_state')['settings']
                assert current['openSeconds']==settings['openSeconds'] and current['closedSeconds']==settings['closedSeconds']
                rows.append(dict(slide=slide,magazine=magazine,duration=value['profile']['duration'],tracks=len(value['profile']['tracks'])))
            project=folder/(slide+'.project.json')
            was_dirty=bool(command('esp_project')['unsavedWeapons'])
            edit('export',dict(kind='project',path=str(project)))
            edit('import_project',dict(path=str(project)));ready()
            reopened=command('operating_setup_state')['settings']
            assert reopened['version']==2 and reopened['openSeconds']==settings['openSeconds'] and reopened['closedSeconds']==0
            assert bool(command('esp_project')['unsavedWeapons'])==was_dirty, 'Import changed the committed-authoring baseline'
            print('PASS',slide,'five magazines, complete follower curves, physical magazine data, sounds, reopen',flush=True)
        if save and command('esp_project')['unsavedWeapons']:
            job=edit('save_project',dict(note='Use source-time windows for all three 1911 slides'))
            while True:
                state=command('pack_job')
                if state.get('id')==job['id'] and state.get('phase') in ('complete','failed'):break
                time.sleep(.5)
            assert state['phase']=='complete',state
            report=state['result']['packReports'][weapon]
            assert report['saved']==15 and report['failed']==0 and not report['exceptions'],report
            (folder/'save.json').write_text(json.dumps(state,indent=2))
        (folder/'result.json').write_text(json.dumps(dict(ok=True,saved=save,configurations=rows),indent=2))
    print('Evidence:',folder,flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--save',action='store_true');run(parser.parse_args().save)
