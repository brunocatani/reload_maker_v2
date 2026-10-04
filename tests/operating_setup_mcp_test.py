"""Explicit native role setup preserves tested motion and survives project reopen."""
import argparse
import json
from pathlib import Path
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit
from server import command

def simulation():
    value=json.loads(Path(edit('export',{'kind':'simulation'})['path']).read_text())
    for key in ('operatingSetup','authoredCurves','motionSource'):value.pop(key,None)
    return value

def run(names):
    work=ROOT/'local/operating-setup-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=ready()['project'];original_bytes=Path(original).read_bytes();results=[]
    try:
        for name in names:
            edit('testbed',{'name':name});ready();project=Path(ready()['project']);saved=project.read_bytes()
            try:
                edit('workspace',{'index':5});ready();before=simulation();setup=command('operating_setup_state')['settings']
                bad=dict(setup,recipeId='0'*64)
                try:edit('operating_setup',dict(operation='save',settings=bad))
                except RuntimeError:pass
                else:raise AssertionError('Stale source setup applied')
                edit('save_project',{'path':str(work/(name+'-read.project.json'))})
                inspected=json.loads((work/(name+'-read.project.json')).read_text())
                assert all(isinstance(p,dict) and 'step' in p for p in inspected['authoringRoadmaps'].values()),'Read/rejected edit inserted a null roadmap'
                edit('operating_setup',dict(operation='save',settings=setup));ready()
                assert command('simulation_state')['active'],command('simulation_state')
                after=simulation();assert after==before,(name,'Explicit preset roles changed source behavior')
                edit('save_project',{'path':str(work/(name+'.project.json'))})
                edit('open_project',{'path':str(work/(name+'.project.json'))});ready()
                assert command('operating_setup_state')['saved'] and simulation()==before
                bad=dict(setup,recipeId='0'*64)
                try:edit('operating_setup',dict(operation='save',settings=bad))
                except RuntimeError:pass
                else:raise AssertionError('Stale source setup applied')
                assert command('operating_setup_state')['settings']==setup
                edit('operating_setup',dict(operation='reset'));ready();assert simulation()==before
                results.append(name);print('PASS',name,flush=True)
            finally:
                project.write_bytes(saved);command('open_project',{'path':str(project)},command('state')['revision']);ready()
                edit('save_project',{'path':str(project)});project.write_bytes(saved)
        (work/'result.json').write_text(json.dumps(dict(ok=True,testbeds=results),indent=2));print('Evidence:',work,flush=True)
    finally:
        Path(original).write_bytes(original_bytes);command('open_project',{'path':original},command('state')['revision']);ready()
        edit('save_project',{'path':original});Path(original).write_bytes(original_bytes)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--testbed',action='append')
    run(parser.parse_args().testbed or ['glock','ru556','m1a','timberwolf','l118a2','thanatos','srep-service','srep-ar10','srep-honeybadger','srep-xm16e1'])
