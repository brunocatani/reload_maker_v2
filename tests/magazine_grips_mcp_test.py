"""Exercise the configured Glock grip library in the running native editor.

Runs on the Glock testbed, which assigns a fit to all seven magazines. The drum
has its own fit: the drum exclusion was removed when the owner authored one
(implementation plan, "Removed the drum model-name exclusion").
Preserves its configuration, camera, and project path; no new grips are created.
"""
import hashlib
import json
import math
import sys
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp'))
from server import command
OUT=ROOT/'evidence/magazine-grips/regression';OUT.mkdir(parents=True,exist_ok=True)
def edit(method,params):return command(method,params,command('revision')['revision'])
def op(name,**params):return edit('simulate',dict(operation=name,**params))
def ready():
    for _ in range(1000):
        state=command('state')
        if not state['busy']:
            assert not state['lastError'],state['lastError'];return state
        time.sleep(.03)
    raise AssertionError('Assembly timeout')
def bones(pose):return {x['name']:x['world'] for x in pose['bones']}
def matrix(q):
    x,y,z,w=q
    return [[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
            [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
            [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]]
def mv(m,v):return [sum(a*b for a,b in zip(row,v)) for row in m]
def relative(part,hand):
    inverse=list(zip(*matrix(part['rotation'])))
    position=mv(inverse,[a-b for a,b in zip(hand['translate'],part['translate'])])
    rotation=[sum(inverse[i][k]*matrix(hand['rotation'])[k][j] for k in range(3)) for i in range(3) for j in range(3)]
    return position+rotation
def check_hand(grip):
    state=command('simulation_state');assert state['magazineGrip']['applied'],state['magazineGrip']
    pose=command('pose');hand=bones(pose)['LArm_Hand' if grip['pose']['hand']=='left' else 'RArm_Hand']
    part=next(p['world'] for p in pose['parts'] if p['name']=='WeaponMagazine')
    placement=grip['pose']['placement']['handInAnchor']
    actual=relative(part,hand);expected=placement['translate']+placement['rotate']
    assert max(abs(a-b) for a,b in zip(actual,expected))<.003,'Wrist stopped following its magazine-local placement'
    return pose

initial=ready();edit('testbed',{'name':'glock'});initial=ready()
snapshot=OUT/'original.project.json';edit('save_project',{'path':str(snapshot)})
pose_hashes={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (ROOT/'workspace/hand-poses').glob('*.json')}
slot=next(s['key'] for s in initial['slots'] if s['name']=='Magazine')
cases=[]
try:
    edit('workspace',{'index':5});ready()
    for mod in [0x1000fa4,0x1000fa5,0x100176f,0x1001770,0x1001779,0x100177a,0x1002e6c]:
        # Selecting the already-installed part is rejected as a no-op edit.
        if next(x['selected'] for x in command('state')['slots'] if x['key']==slot)!=str(mod):
            edit('configure',{'slot':slot,'mod':str(mod)});ready()
        op('reset')
        sim=command('simulation_state');gid=sim['magazineGrip']['id'];assert gid
        grip=json.loads((ROOT/'workspace/part-grips'/f'{gid}.json').read_text(encoding='utf-8'))
        original=json.loads((ROOT/'workspace/hand-poses'/f"{grip['sourcePoseId']}.json").read_text(encoding='utf-8'))
        for a,b in zip(grip['pose']['fingerLocalTransforms'],original['fingerLocalTransforms']):
            assert max(abs(x-y) for x,y in zip(a['rotate'],b['rotate']))<2e-6
            assert a['translate']==b['translate']
        before=bones(command('pose'));op('drop_magazine');grab=op('grab_magazine');check_hand(grip)
        offset=grab['magazine']['offset']
        op('move_magazine',position=[offset[0]+100,offset[1]+10,offset[2]-50])
        after=bones(check_hand(grip));other='RArm_Hand' if grip['pose']['hand']=='left' else 'LArm_Hand'
        assert math.dist(before[other]['translate'],after[other]['translate'])<.003
        assert before['Weapon']==after['Weapon'],'Hand preview moved the weapon'
        rail=grab['magazineRail'];entry=[x*rail['travel'] for x in rail['outwardInWeapon']]
        op('move_magazine',position=entry);assert command('simulation_state')['magazine']['phase']=='Full magazine on rail'
        op('move_magazine',distance=rail['travel']*.4);check_hand(grip)
        seated=op('move_magazine',distance=0);assert not seated['magazineGrip']['applied']
        assert seated['ammunition']['magazineRounds']==seated['ammunition']['capacity']
        cases.append({'magazine':hex(mod),'gripId':gid,'capacity':sim['ammunition']['capacity']})
    edit('configure',{'slot':slot,'mod':str(0x1001770)});ready()
    edit('playback',{'mode':0});op('reload');samples=[]
    for _ in range(8):
        op('step',seconds=.1);samples.append(command('pose'))
    weapon=bones(samples[0])['Weapon']
    assert math.dist(weapon['translate'],[0,0,0])<.001
    for sample in samples:assert math.dist(bones(sample)['Weapon']['translate'],weapon['translate'])<.001
    assert math.dist(bones(samples[0])['LArm_Hand']['translate'],bones(samples[-1])['LArm_Hand']['translate'])>.1
    seconds=command('simulation_state')['seconds'];edit('author_reload_grips',{});s=command('state')
    assert s['workspace']==2 and s['motionMode']==0 and seconds<=s['seconds']<=seconds+.2
    # Same native frame, two world presentations: magazine-relative wrist must agree.
    fixed=command('pose');fixed_hand=bones(fixed)['LArm_Hand']
    fixed_part=next(x['world'] for x in fixed['parts'] if x['name']=='WeaponExtra3')
    edit('playback',{'mode':1});native=command('pose')
    native_part=next(x['world'] for x in native['parts'] if x['name']=='WeaponExtra3')
    assert max(abs(a-b) for a,b in zip(relative(fixed_part,fixed_hand),relative(native_part,bones(native)['LArm_Hand'])))<.003
    assert math.dist(bones(native)['Weapon']['translate'],[0,0,0])>.1,'Native-motion option was lost'
    edit('playback',{'mode':0});edit('workspace',{'index':5});ready()
    saved=OUT/'assigned-grips.project.json';edit('save_project',{'path':str(saved)})
    assigned=command('state')['magazineGrabGrips'];edit('open_project',{'path':str(saved)});ready()
    assert command('state')['magazineGrabGrips']==assigned
    exported=edit('export',{'kind':'reload_bundle'});bundle=json.loads(Path(exported['path']).read_text())
    ids={g['pose']['id'] for g in bundle['grips']}
    bindings=bundle['magazineGrabBindings'];assert len(bindings)==7,len(bindings)
    assert all(x['gripId'] in ids for x in bindings)
    assert pose_hashes=={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (ROOT/'workspace/hand-poses').glob('*.json')}
    (OUT/'validated.json').write_text(json.dumps({'cases':cases,'fixedReloadSamples':len(samples),'bundle':exported['path'],'sourcePosesUnchanged':True},indent=2))
    print('PASS seven magazine grips including the drum, free/rail following, reach and hand independence, fixed/native reload authoring, persistence and self-contained export')
finally:
    edit('open_project',{'path':str(snapshot)});ready();edit('save_project',{'path':initial['project']})
