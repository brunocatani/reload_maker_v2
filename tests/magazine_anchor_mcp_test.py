"""Check rendered wrists against their exact saved magazine anchor, not applied flags."""
import json,math,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'mcp'))
from server import command
OUT=ROOT/'local/magazine-fit-fix';OUT.mkdir(parents=True,exist_ok=True)
def ready():
 for _ in range(2400):
  s=command('state')
  if not s['busy']:
   assert not s['lastError'],s['lastError'];return s
  time.sleep(.025)
 raise AssertionError('Source timed out')
def edit(m,**p):return command(m,p,ready()['revision'])
def op(operation,**p):return edit('simulate',operation=operation,**p)
def rotation(q):
 x,y,z,w=q;return [[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],[2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],[2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]]
def relative(root,point):
 delta=[a-b for a,b in zip(point,root['translate'])]
 return [sum(a*b for a,b in zip(row,delta))/root['scale'][0] for row in zip(*rotation(root['rotation']))]
def check_fit(grip):
 scene=command('pose');p=grip['placement'];prefix='L' if grip['hand']=='left' else 'R'
 root=next(x['world'] for x in scene['parts'] if x['name']==p['anchorName'] and x['ownerRecord']==p['ownerRecord'])
 wrist=next(b['world'] for b in scene['bones'] if b['name']==prefix+'Arm_Hand')
 actual=relative(root,wrist['translate']);assert math.dist(actual,p['handInAnchor']['translate'])<.015,(grip['hand'],p['ownerRecord'],actual,p['handInAnchor']['translate'])
 inv=list(zip(*rotation(root['rotation'])));actual_rotation=[sum(a*b for a,b in zip(row,col)) for row in inv for col in zip(*rotation(wrist['rotation']))]
 assert max(abs(a-b) for a,b in zip(actual_rotation,p['handInAnchor']['rotate']))<.004
 for expected in grip['fingerLocalTransforms']:
  actual=next(b['local'] for b in scene['bones'] if b['name']==expected['bone'])
  assert math.dist(actual['translate'],expected['translate'])<.015
  assert max(abs(a-b) for a,b in zip(sum(rotation(actual['rotation']),[]),expected['rotate']))<.004
 return scene
results=[]
for title,expected in [('M1A',4)]:
 project=ROOT/'workspace'/f'{title}-native-reload.project.json';edit('open_project',path=str(project));ready();edit('workspace',index=5);ready();op('reset')
 slots=ready()['slots'];mag=next(s for s in slots if s['name']=='Magazine');assert len(mag['choices'])==expected
 for c in mag['choices']:
  op('reset');current=next(s for s in ready()['slots'] if s['name']=='Magazine')
  if current['selected']!=c['id']:edit('configure',slot=current['key'],mod=c['id']);ready()
  for hand in ['left','right']:
   op('reset');edit('interaction_hand',hand=hand);op('drop_magazine');held=op('grab_magazine');assert held['magazineGrip']['applied']
   grip=json.loads((ROOT/'workspace/part-grips'/(held['magazineGrip']['id']+'.json')).read_text())['pose'];check_fit(grip)
   rail=held['magazineRail'];op('move_magazine',position=[v*rail['travel']*.8 for v in rail['outwardInWeapon']]);op('move_magazine',distance=rail['travel']*.55);scene=check_fit(grip)
   if title=='M1A':
    sim=json.loads(Path(edit('export',kind='simulation')['path']).read_text());r=sim['sourceRecipe'];f=sim['loadedMagazineFrame'];root=r['tracks'][sim['magazineTrack']]['poses'][f]
    source={'translate':root[:3],'rotation':root[3:7],'scale':root[7:]};live=next(p['world'] for p in scene['parts'] if p['name']=='WeaponMagazine')
    for t in r['tracks']:
     if t['anchor']['name'] not in ('WeaponMagazineChild1','WeaponMagazineChild2','WeaponMagazineChild3'):continue
     child=next(p['world'] for p in scene['parts'] if p['name']==t['anchor']['name'])
     assert math.dist(relative(live,child['translate']),relative(source,t['poses'][f][:3]))<.015,'Detached cartridge retained a chamber/feed pose'
   op('reset')
  if title=='M1A':
   sim=command('simulation_state')
   for _ in range(sim['ammunition']['capacity']):op('fire');op('step',seconds=1)
   empty=op('mag_out');op('step',seconds=.2)
   assert all(v['flags']&1 for v in command('simulation_state')['magazineShapes'] if v['cartridge']),'Empty magazine shows cartridges'
   op('reset')
  results.append((title,c['record']));print('PASS magazine anchor / joints / payload',title,c['record'],flush=True)
(OUT/'anchor-validation.json').write_text(json.dumps(results,indent=2))
print('PASS',len(results),'magazines, both hands, free carry and insertion rail',flush=True)
