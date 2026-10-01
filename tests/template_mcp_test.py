"""Explicit template assignment must preserve the five proven source behaviors."""
import argparse
import json
from pathlib import Path
import sys
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp'))
from server import command


def ready():
    end=time.monotonic()+120
    while time.monotonic()<end:
        state=command('state')
        if not state['busy']:
            assert not state['lastError'],state['lastError'];return state
        time.sleep(.1)
    raise AssertionError('Source capture did not finish')


def edit(method,params):return command(method,params,ready()['revision'])


def simulation():
    state=command('simulation_state');assert state['active'] and not state['error'],state
    return json.loads(Path(edit('export',{'kind':'simulation'})['path']).read_text())


def run(names):
    work=ROOT/'local/template-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=ready()['project'];original_bytes=Path(original).read_bytes();results=[]
    try:
        for name in names:
            edit('testbed',{'name':name});ready();project=Path(ready()['project']);saved=project.read_bytes()
            try:
                edit('workspace',{'index':5});ready();before=simulation()
                state=command('roadmap');expected=state['template'];assert len(state['templates'])==5
                assigned=edit('roadmap',dict(operation='template',id=expected));ready()
                assert assigned['templateAssigned'] and assigned['template']==expected
                after=simulation();assert after==before,(name,'Template assignment changed the existing source behavior')
                edit('save_project',{'path':str(work/(name+'.project.json'))})
                edit('open_project',{'path':str(work/(name+'.project.json'))});ready()
                reopened=command('roadmap');assert reopened['templateAssigned'] and reopened['template']==expected
                assert simulation()==before,(name,'Reopening an assigned template changed behavior')
                try:edit('roadmap',dict(operation='template',id='unsupported.template'))
                except RuntimeError:pass
                else:raise AssertionError('Unknown behavior template accepted')
                results.append(dict(name=name,template=expected));print('PASS',name,expected,flush=True)
            finally:
                project.write_bytes(saved);edit('open_project',{'path':str(project)});ready()
                edit('save_project',{'path':str(project)});project.write_bytes(saved)
        (work/'result.json').write_text(json.dumps(dict(ok=True,results=results),indent=2));print('Evidence:',work)
    finally:
        Path(original).write_bytes(original_bytes);edit('open_project',{'path':original});ready()
        edit('save_project',{'path':original});Path(original).write_bytes(original_bytes)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--testbed',action='append')
    run(parser.parse_args().testbed or ['glock','ru556','m1a','timberwolf','thanatos'])
