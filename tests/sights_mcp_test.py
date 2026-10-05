"""Actual Glock attachment remaps through the editor's local MCP command adapter."""
import json
import math
import shutil
import sys
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp'))
from server import command
OUT=ROOT/'evidence/sights'
OUT.mkdir(parents=True,exist_ok=True)

def edit(method,params):
    return command(method,params,command('revision')['revision'])

def ready():
    for _ in range(600):
        s=command('state')
        if not s['busy']:
            assert not s['lastError'],s['lastError']
            return s
        time.sleep(.05)
    raise AssertionError('Assembly did not finish')

def op(name,**params):
    return edit('simulate',dict(operation=name,**params))

def finished():
    for _ in range(300):
        s=command('simulation_state')
        if s['action']=='Ready':return s
        attached()
        op('step',seconds=.03)  # commands advance simulated time only by stepping
    raise AssertionError('Simulation did not finish')

def attached():
    remaps=command('diagnostics')['attachmentRemaps']
    assert len(remaps)==1,remaps
    r=remaps[0]
    assert r['point']=='P-Glock19xSights'
    # Independently inspected source nodes/connection are identity transforms.
    # The branch must equal its slide parent through translation AND rotation.
    for field in ('translate','rotation','scale'):
        assert all(abs(a-b)<.0002 for a,b in zip(r['world'][field],r['parentWorld'][field])),r
    return r

def shot(name,view='side'):
    r=command('render',{'view':view,'fit':True,'zoom':1.6})
    shutil.copyfile(r['path'],OUT/(name+'.png'))
    (OUT/(name+'.json')).write_text(json.dumps(attached(),indent=2),encoding='utf-8')

state=ready()
edit('workspace',{'index':5});ready()
sights=next(s for s in state['slots'] if s['name']=='Sights')
receiver=next(s for s in state['slots'] if s['name']=='Receiver')
original={sights['key']:sights['selected'],receiver['key']:receiver['selected']}
evidence=[]
receivers=[c for c in receiver['choices'] if c['editorId'] in ('mod_Glock19x_Receiver_Standard','mod_Glock19x_Receiver_Agency_Standard')]
assert len(receivers)==2
for body in receivers:
    current=next(s for s in command('state')['slots'] if s['key']==receiver['key'])
    if current['selected']!=body['id']:
        edit('configure',{'slot':receiver['key'],'mod':body['id']});ready()
    for choice in sights['choices']:
        current=next(s for s in command('state')['slots'] if s['key']==sights['key'])
        if current['selected']!=choice['id']:
            edit('configure',{'slot':sights['key'],'mod':choice['id']});ready()
        op('reset');closed=attached()
        op('pull',progress=1);opened=attached()
        assert math.dist(closed['world']['translate'],opened['world']['translate'])>2.5,'Sight did not move'
        assert math.dist(closed['attachmentWorld']['translate'],opened['attachmentWorld']['translate'])<.001,'Remap moved the complete attachment'
        evidence.append({'receiver':body['editorId'],'sights':choice['editorId'],'closed':closed,'open':opened})
        shot(body['editorId']+'-'+choice['editorId']+'-open','iso' if 'Mount_' in choice['editorId'] else 'side')
        op('release');finished();attached()

for slot,mod in original.items():
    if next(s for s in command('state')['slots'] if s['key']==slot)['selected']!=mod:
        edit('configure',{'slot':slot,'mod':mod});ready()
op('reset');op('fire');finished();op('reload');finished()
# Check the existing native preview, not only the simulation override.
edit('workspace',{'index':5});s=ready()
fire=next(a for a in s['actions'] if 'Fire' in a['name'] and 'single' in a['name'])
edit('action',{'index':fire['index']});duration=command('state')['duration']
for fraction in (0,.15,.45,.8,1):
    edit('playback',{'seconds':duration*fraction,'mode':1});attached()
edit('workspace',{'index':4});ready();duration=command('reload')['duration']
for fraction in (0,.25,.5,.85,.91,.98,1):
    edit('playback',{'seconds':duration*fraction});attached()
edit('workspace',{'index':5});ready();op('reset')
for _ in range(command('simulation_state')['ammunition']['capacity']):
    op('fire');finished()
assert command('simulation_state')['ammunition']['slideLocked']
shot('standard-locked-open')
op('drop_magazine');attached();op('slide_release');finished();attached()
op('reset');shot('handoff-ready')
edit('save_project',{'path':str(ROOT/'workspace/Glock19x-simulation.project.json')})
(OUT/'validated-variants.json').write_text(json.dumps(evidence,indent=2),encoding='utf-8')
print('PASS: all five sights on standard/Agency slides, stationary attachment roots, manual rack/return, native fire/reload, reload graph, empty lock/release and saved clean handoff')
