"""Capture visible magazine geometry, generate hulls and cook through Maker."""
import argparse
import json
from pathlib import Path
import sys
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp'))
from server import command


def ready(method='state'):
    deadline=time.monotonic()+120
    while time.monotonic()<deadline:
        state=command(method)
        if not state.get('busy',False):
            assert not state.get('error',state.get('lastError','')),state
            return state
        time.sleep(.1)
    raise AssertionError('Editor did not finish the native operation')


def edit(method,params):return command(method,params,ready()['revision'])


def finish(job):
    deadline=time.monotonic()+120
    while time.monotonic()<deadline:
        state=command('pack_job');assert state['id']==job['id']
        if state['phase'] in ('complete','failed'):
            assert state['phase']=='complete',state
            return state['result']
        time.sleep(.2)
    raise AssertionError('Native asset job timed out')


def reference(path):return dict(path=str(path))


def main(names):
    work=ROOT/'local/visible-geometry-suite'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=command('export',{'kind':'project'},ready()['revision'])['path'];edit('export',{'kind':'project','path':original});original_bytes=Path(original).read_bytes()
    results=[]
    try:
        for name in names or ['current']:
            if name!='current':edit('testbed',{'name':name});ready()
            project=Path(command('export',{'kind':'project'},ready()['revision'])['path']);saved=project.read_bytes()
            folder=work/name;folder.mkdir()
            try:
                edit('workspace',{'index':9});state=command('collision_state')
                target=next(t['index'] for t in state['targets'] if t['name'].startswith('Magazine '))
                edit('collisions',{'operation':'select','target':target})
                prepared=finish(edit('capture_prepared_part',{'output':str(folder/'prepared.nif')}))
                assert not prepared['engineCooked'] and prepared['shapes']>0
                edit('collisions',{'operation':'defaults'});ready('collision_state')
                hulls=folder/'hulls.json';edit('collisions',{'operation':'export-hulls','path':str(hulls)})
                job=folder/'job.json';job.write_text(json.dumps(dict(format='reload-maker-world-object-job',version=1,
                    geometry=reference(folder/'prepared.nif'),collision=reference(hulls))))
                cooked=finish(edit('pack_job',dict(operation='cook',job=str(job),output=str(folder/'finished.nif'))))
                assert cooked['engineCooked']
                if name in ('current','srep-service'):
                    sources=command('collision_state')['sources'];assert len(sources)>1
                    excluded=sources[0]
                    edit('collisions',{'operation':'piece','index':0,'included':False})
                    selection=folder/'selected.collision.json';edit('collisions',{'operation':'export','path':str(selection)})
                    edit('collisions',{'operation':'piece','index':0,'included':True})
                    edit('collisions',{'operation':'open','path':str(selection)})
                    assert not command('collision_state')['sources'][0]['included']
                    selected=finish(edit('capture_prepared_part',{'output':str(folder/'selected.nif')}))
                    assert selected['shapes']==prepared['shapes']-1
                    edit('collisions',{'operation':'generate'});selected_hulls=ready('collision_state')
                    assert all(h['source']!=excluded['key'] for h in selected_hulls['hulls'])
                results.append(dict(testbed=name,prepared=prepared,cooked=cooked))
                print('PASS',name,prepared['shapes'],'shapes,',prepared['triangles'],'triangles,',cooked['hulls'],'hulls',flush=True)
            finally:
                project.write_bytes(saved);edit('import_project',{'path':str(project)});ready()
        (work/'result.json').write_text(json.dumps(dict(ok=True,results=results),indent=2))
        print('Evidence:',work,flush=True)
    finally:
        Path(original).write_bytes(original_bytes);edit('import_project',{'path':original});ready()


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--testbed',action='append')
    main(parser.parse_args().testbed)
