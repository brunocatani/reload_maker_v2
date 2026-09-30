"""Authored operating/magazine hands: native transform, isolation and persistence checks."""
import json,math,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp'))
from server import command
OUT=ROOT/'evidence/interaction-grips';OUT.mkdir(parents=True,exist_ok=True)
def edit(m,**p):
    for attempt in range(5):
        try:return command(m,p,command('state')['revision'])
        except RuntimeError as e:
            if 'Stale' not in str(e) or attempt==4:raise
def op(operation,**p):return edit('simulate',operation=operation,**p)
def ready():
    deadline=time.monotonic()+45
    while time.monotonic()<deadline:
        s=command('state')
        if not s['busy']:
            assert not s['lastError'],s['lastError'];return s
        time.sleep(.03)
    raise AssertionError('Load stalled')
def matrix(q):
    x,y,z,w=q
    return [1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w),2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w),2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]
def frame(t):return {'rotate':matrix(t['rotation']),'translate':t['translate'],'scale':t['scale'][0]}
def relative(a,b):
    ra=a['rotate'];rb=b['rotate'];d=[b['translate'][i]-a['translate'][i] for i in range(3)]
    return {'translate':[sum(ra[k*3+i]*d[k] for k in range(3))/a['scale'] for i in range(3)],
            'rotate':[sum(ra[k*3+i]*rb[k*3+j] for k in range(3)) for i in range(3) for j in range(3)],'scale':b['scale']/a['scale']}
def near(a,b):
    assert math.dist(a['translate'],b['translate'])<.006,(a,b)
    assert max(abs(x-y) for x,y in zip(a['rotate'],b['rotate']))<.003,(a,b)
def posed_hand(hand):return next(b for b in command('pose')['bones'] if b['name']==('LArm_Hand' if hand=='left' else 'RArm_Hand'))
def check_fit(g):
    pose=command('pose');placement=g['pose']['placement'];hand=g['pose']['hand']
    wrist=next(b for b in pose['bones'] if b['name']==('LArm_Hand' if hand=='left' else 'RArm_Hand'))
    anchor=next(p for p in pose['parts'] if p['name']==placement['anchorName'] and p['kind']==placement['anchorKind'] and p['ownerRecord'].lower()==placement['ownerRecord'].lower())
    near(relative(frame(anchor['world']),frame(wrist['world'])),placement['handInAnchor'])
    for i,expected in enumerate(g['pose']['fingerLocalTransforms']):
        name=('L' if hand=='left' else 'R')+f'Arm_Finger{i//3+1}{i%3+1}'
        actual=next(b['local'] for b in pose['bones'] if b['name']==name);near(frame(actual),expected)
def export(kind):
    r=edit('export',kind=kind);return json.loads(Path(r['path']).read_text(encoding='utf-8'))
original=ready()['project'];backup=OUT/'original.project.json';edit('save_project',path=str(backup))
results={}
try:
    for name,count,native_hand in [('timberwolf',2,'right'),('m1a',4,'left'),('ru556',8,'left')]:
        edit('testbed',name=name);s=ready();path=s['project'];saved=OUT/(name+'.project.json');edit('save_project',path=str(saved))
        saved_bytes=saved.read_bytes()
        try:
            op('reset');payload=export('simulation');groups=payload['authoredInteractions']['operatingGroups']
            assert len(groups)==1 and groups[0]['hand']==native_hand
            unavailable='right' if native_hand=='left' else 'left'
            edit('interaction_hand',hand=unavailable);assert command('state')['interaction']['canGrab']
            dynamic=op('pull',progress=.5);assert not dynamic['interaction']['applied'] and not dynamic['interaction']['authored'];op('release',cancel=True)
            assert payload['authoredInteractions']['otherHandPolicy']=='dynamic'
            for hand in [native_hand]:
                edit('interaction_hand',hand=hand);idle=command('simulation_state');assert idle['interaction']['canGrab'] and not idle['interaction']['markerVisible']
                chosen=next(g for g in groups if g['hand']==hand);op('reset')
                other='right' if hand=='left' else 'left';stationary=frame(posed_hand(other)['world']);rest=frame(posed_hand(hand)['world'])
                for progress in [.15,.5,1,.7,.2]:
                    held=op('pull',progress=progress);assert held['interaction']['applied'] and not held['interaction']['error'] and not held['interaction']['markerVisible']
                    check_fit(chosen['grip']);near(frame(posed_hand(other)['world']),stationary)
                op('release',cancel=True);assert not command('simulation_state')['interaction']['applied'];near(frame(posed_hand(hand)['world']),rest)
                op('fire');op('step',seconds=3);assert not command('simulation_state')['interaction']['applied']
            slots=command('state')['slots'];slot=next(a for a in slots if a['name']=='Magazine');assert len(slot['choices'])==count
            checked=[]
            for choice in slot['choices']:
                op('reset')
                if next(a for a in command('state')['slots'] if a['key']==slot['key'])['selected']!=choice['id']:
                    edit('configure',slot=slot['key'],mod=choice['id']);ready()
                bundle=export('reload_bundle');assert bundle['version']==2
                fits={b['hand']:next(g for g in bundle['grips'] if g['pose']['id']==b['gripId']) for b in bundle['magazineGrabBindings'] if b['ownerRecord']==choice['record']}
                assert set(fits)=={'left','right'}
                for hand in ['left','right']:
                    edit('interaction_hand',hand=hand);op('reset');op('drop_magazine');state=op('grab_magazine');assert state['magazineGrip']['applied']
                    assert not state['interaction']['markerVisible'];check_fit(fits[hand])
                    rail=state['magazineRail'];axis=rail['outwardInWeapon'];op('move_magazine',position=[v*rail['travel']*.7 for v in axis]);op('move_magazine',distance=rail['travel']*.3);check_fit(fits[hand]);op('move_magazine',distance=0)
                    assert command('simulation_state')['magazinePresent'];checked.append((choice['record'],hand))
            # Saving replaces the native operating assignment, preserving both magazine fits.
            edit('interaction_hand',hand=native_hand);before=export('simulation')['authoredInteractions']['operatingGroups'];magazines=command('state')['magazineGrabGrips'];edit('configure_operating_grip');edit('use_operating_grip');edit('workspace',index=5);ready()
            after=export('simulation')['authoredInteractions']['operatingGroups']
            assert before[0]['grip']['pose']['id']!=after[0]['grip']['pose']['id'] and command('state')['magazineGrabGrips']==magazines
            edit('save_project',path=str(OUT/(name+'-replacement.project.json')));edit('open_project',path=str(OUT/(name+'-replacement.project.json')));ready();assert export('simulation')['authoredInteractions']['operatingGroups']==after
            results[name]=checked;print('PASS',name,'native operating hand and',len(checked),'magazine-hand fits',flush=True)
        finally:
            saved.write_bytes(saved_bytes)
            edit('open_project',path=str(saved));ready();edit('save_project',path=path)
    edit('testbed',name='glock');ready();op('reset');assert not command('state')['interaction']['authored']
    try:edit('interaction_hand',hand='right');raise AssertionError('Right-hand Glock reload accepted')
    except RuntimeError as error:assert 'Glock' in str(error)
    op('pull',progress=1);op('release');op('step',seconds=1)
    assert command('simulation_state')['ammunition']['chambered']
    (OUT/'validated.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    print('PASS hand/part transforms, exact finger joints, stationary support hand, assignment replacement, restart persistence, exports and Glock dynamic grab')
finally:
    edit('open_project',path=str(backup));ready();edit('save_project',path=original)
