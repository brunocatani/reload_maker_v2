"""Dynamic operating grabs with unchanged authored magazine wrists and fingers."""
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
def run():
    original=ready()['project']
    edit('save_project',path=original)
    projects=['Glock19x-magazine-grips','RU556-native-reload','M1A-native-reload','Timberwolf-native-reload',
        'L118A2-native-reload','Thanatos-native-reload','MK18-native-reload','MK18-Empire-native-reload',
        'srep-service-native-reload','srep-honeybadger-native-reload','srep-ar10-native-reload','srep-xm16e1-native-reload']
    results=[]
    try:
        for name in projects:
            canonical=ROOT/'workspace'/(name+'.project.json')
            edit('open_project',path=str(canonical));ready();edit('workspace',index=5);ready();op('reset')
            old=json.loads(canonical.read_text(encoding='utf-8'));payload=export('simulation')
            policy=payload['authoredInteractions']
            assert policy['operatingPosePolicy']=='dynamic' and policy['operatingGroups']==[]
            points=command('grab_locations_state')['profile']
            hands=['left'] if name.startswith('Glock') else ['left','right']
            for hand in hands:
                edit('interaction_hand',hand=hand);op('reset')
                scene=command('pose');baseline={b['name']:b['world'] for b in scene['bones'] if b['name'].endswith('Arm_Hand')}
                fingers={b['name']:b['local'] for b in scene['bones'] if 'Arm_Finger' in b['name']}
                for progress in [.15,.5,1,.7,.2]:
                    state=op('pull',progress=progress)['interaction']
                    assert state['canGrab'] and state['posePolicy']=='dynamic' and not state['authored'] and not state['applied'] and not state['error']
                    for b in command('pose')['bones']:
                        if b['name'] in baseline:near(frame(b['world']),frame(baseline[b['name']]))
                        if b['name'] in fingers:near(frame(b['local']),frame(fingers[b['name']]))
                op('release',cancel=True);op('reset')
                # Saved magazine poses still control the held/inserting mesh.
                op('drop_magazine');held=op('grab_magazine');assert held['magazineGrip']['applied']
                grip=json.loads((ROOT/'workspace/part-grips'/(held['magazineGrip']['id']+'.json')).read_text(encoding='utf-8'))
                check_fit(grip);rail=held['magazineRail'];op('move_magazine',position=[v*rail['travel']*.7 for v in rail['outwardInWeapon']]);op('move_magazine',distance=rail['travel']*.4);check_fit(grip)
                op('move_magazine',distance=0);op('reset')
            assert command('grab_locations_state')['profile']==points
            # Existing operating assignments survive as library references but
            # cannot regain runtime authority after saving and reopening.
            scratch=OUT/(name+'-dynamic.project.json');edit('save_project',path=str(scratch))
            saved=json.loads(scratch.read_text(encoding='utf-8'))
            for key in ['operatingGrips','magazineGrabGrips','magazineGrabDefaults','grabLocations']:assert saved.get(key,{})==old.get(key,{})
            edit('open_project',path=str(scratch));ready();edit('workspace',index=5);ready();op('reset')
            assert export('simulation')['authoredInteractions']['operatingPosePolicy']=='dynamic'
            # Restore only the canonical pointer, preserving the original file.
            edit('open_project',path=str(canonical));ready();edit('save_project',path=str(canonical))
            results.append(name);print('PASS',name,'dynamic operating hands; authored magazine transforms; points and persistence',flush=True)
        (OUT/'dynamic-validated.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    finally:
        edit('open_project',path=original);ready()
    print('PASS',len(results),'testbeds; dynamic operation and authored magazines',flush=True)

if __name__=='__main__':run()
