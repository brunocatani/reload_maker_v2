"""Native L118A2/Thanatos loops and frozen source exports for DIAMOND."""
import json,math,sys,time,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'mcp'))
from server import command
OUT=ROOT/'local/new-rifles/validated';OUT.mkdir(parents=True,exist_ok=True)
def ready():
    until=time.monotonic()+60
    while time.monotonic()<until:
        state=command('state')
        if not state['busy']:
            assert not state['lastError'],state['lastError'];return state
        time.sleep(.025)
    raise AssertionError('Native source did not finish')
def edit(method,**params):
    for attempt in range(5):
        try:return command(method,params,ready()['revision'])
        except RuntimeError as error:
            if 'Stale' not in str(error) or attempt==4:raise
def op(operation,**params):return edit('simulate',operation=operation,**params)
def export(kind,name):
    path=Path(edit('export',kind=kind)['path']);target=OUT/(name+'-'+kind+'.json');shutil.copyfile(path,target)
    return json.loads(target.read_text(encoding='utf-8'))
def rotation(q):
    x,y,z,w=q
    return [[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
            [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
            [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]]
def check_fit(grip):
    pose=command('pose');placement=grip['placement'];hand=grip['hand'];prefix='L' if hand=='left' else 'R'
    root=next(p['world'] for p in pose['parts'] if p['name']==placement['anchorName'] and p['ownerRecord']==placement['ownerRecord'])
    wrist=next(b['world'] for b in pose['bones'] if b['name']==prefix+'Arm_Hand')
    inv=list(zip(*rotation(root['rotation'])));delta=[a-b for a,b in zip(wrist['translate'],root['translate'])]
    actual=[sum(a*b for a,b in zip(row,delta))/root['scale'][0] for row in inv]
    assert math.dist(actual,placement['handInAnchor']['translate'])<.008,(hand,actual,placement)
    actual_rotation=[sum(a*b for a,b in zip(row,col)) for row in inv for col in zip(*rotation(wrist['rotation']))]
    assert max(abs(a-b) for a,b in zip(actual_rotation,placement['handInAnchor']['rotate']))<.004
    for i,expected in enumerate(grip['fingerLocalTransforms']):
        actual=next(b['local'] for b in pose['bones'] if b['name']==prefix+f'Arm_Finger{i//3+1}{i%3+1}')
        assert math.dist(actual['translate'],expected['translate'])<.008
        assert max(abs(a-b) for a,b in zip(sum(rotation(actual['rotation']),[]),expected['rotate']))<.004
original=ready()['project'];results={}
try:
    for name,title,manual in [('l118a2','L118A2',True),('thanatos','Thanatos',False)]:
        edit('testbed',name=name);state=ready();canonical=Path(state['project']);edit('save_project',path=str(canonical))
        saved=canonical.read_bytes();scratch=OUT/(title+'.project.json');scratch.write_bytes(saved)
        edit('save_project',path=str(scratch));tested=[]
        try:
            edit('workspace',index=5);ready();slot=next(s for s in ready()['slots'] if s['key']=='382167');assert len(slot['choices'])==2
            for choice in slot['choices']:
                op('reset')
                if next(s for s in ready()['slots'] if s['key']==slot['key'])['selected']!=choice['id']:edit('configure',slot=slot['key'],mod=choice['id']);ready()
                initial=op('reset');assert initial['active'] and not initial['error'] and not initial['magazine']['error']
                capacity=initial['ammunition']['capacity'];assert capacity==(10 if not manual or choice['record'].endswith('000012') else 5)
                assert not initial['ammunition']['automatic']
                stem=title+'-'+choice['record'].split(':')[1]
                simulation=export('simulation',stem);bundle=export('reload_bundle',stem);export('sounds',stem)
                assert simulation['authoredInteractions']['operatingPosePolicy']=='dynamic' and not simulation['authoredInteractions']['operatingGroups']
                points=simulation['authoredInteractions']['grabLocations'];assert points['sourceName']=='WeaponBolt' and len(points['points'])==1 and points['points'][0]['handMask']==3
                assert simulation['magazineInteraction']['inputPath']=='straight'
                # Shared receiver ownership must never classify the receiver,
                # operating handle or trigger as carried magazine members.
                members=simulation['magazineTracks'];assert all(m['name'].startswith('WeaponMagazine') for m in members),members
                for hand in ['right','left']:
                    edit('interaction_hand',hand=hand);op('reset')
                    for progress in [.1,.5,1]:
                        held=op('pull',progress=progress);assert not held['interaction']['applied'] and held['interaction']['posePolicy']=='dynamic'
                    op('release',cancel=True);op('reset')
                bindings={b['hand']:b for b in bundle['magazineGrabBindings'] if b['ownerRecord']==choice['record']};assert set(bindings)=={'left','right'}
                grips={g['pose']['id']:g['pose'] for g in bundle['grips']}
                for hand in ['left','right']:
                    edit('interaction_hand',hand=hand);op('reset');op('drop_magazine');held=op('grab_magazine');assert held['magazineGrip']['applied']
                    check_fit(grips[bindings[hand]['gripId']]);rail=held['magazineRail'];axis=rail['outwardInWeapon'];travel=rail['travel']
                    captured=op('move_magazine',position=[v*travel*.6 for v in axis]);assert captured['magazine']['phase']=='Full magazine on rail'
                    op('move_magazine',distance=travel*.3);check_fit(grips[bindings[hand]['gripId']]);seated=op('move_magazine',distance=0);assert seated['magazinePresent']
                op('reset');op('trigger_down');shot=op('step',seconds=3);assert shot['ammunition']['shotsFired']==1
                if manual:
                    assert shot['boltAction']['spentCase'] and not shot['ammunition']['canFire']
                    threshold=shot['boltAction']['ejectProgress'];op('pull',progress=threshold*.8);op('release');held=op('step',seconds=1);assert held['boltAction']['spentCase']
                    op('pull',progress=1);op('release');held=op('step',seconds=1);assert held['slide']==1 and held['boltAction']['casesEjected']==1
                    op('pull',progress=0);op('release');assert command('simulation_state')['ammunition']['canFire']
                else:
                    op('trigger_up')
                    for _ in range(capacity-1):op('fire');op('step',seconds=2)
                    empty=command('simulation_state');assert empty['ammunition']['slideLocked'] and empty['slide']==1
                    op('drop_magazine');held=op('grab_magazine');rail=held['magazineRail'];op('move_magazine',position=[v*rail['travel']*.5 for v in rail['outwardInWeapon']]);op('move_magazine',distance=0)
                    op('bolt_release');loaded=op('step',seconds=1);assert loaded['ammunition']['canFire'] and not loaded['ammunition']['slideLocked']
                op('reset');op('fire');op('step',seconds=3);op('reload');loaded=op('step',seconds=12);assert loaded['ammunition']['canFire']
                # Freeze a coherent native carrier/installed-root relation for
                # world-model geometry and wrist normalization in the importer.
                edit('workspace',index=2);edit('action',index=1);edit('playback',seconds=0,playing=False,mode=0)
                (OUT/(stem+'-native-seat.json')).write_text(json.dumps(command('pose')),encoding='utf-8')
                edit('workspace',index=5);ready();op('reset')
                tested.append({'record':choice['record'],'capacity':capacity,'model':choice['model']});print('PASS',title,choice['record'],'both hands, native motion, reload and export',flush=True)
            receiver=next(s for s in ready()['slots'] if s['key']=='147460')
            for choice in receiver['choices']:
                if next(s for s in ready()['slots'] if s['key']==receiver['key'])['selected']!=choice['id']:edit('configure',slot=receiver['key'],mod=choice['id']);ready()
                state=op('reset');assert state['active'] and not state['error'] and not state['magazine']['error']
                op('fire');state=op('step',seconds=3);assert state['ammunition']['shotsFired']==1
            results[title]={'magazines':tested,'receivers':len(receiver['choices'])}
        finally:
            scratch.write_bytes(saved);edit('open_project',path=str(scratch));ready();edit('save_project',path=str(canonical))
    (OUT/'validated.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
finally:
    edit('open_project',path=original);ready()
