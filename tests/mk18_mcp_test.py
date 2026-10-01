"""Exercise both ordered MK18 sources and freeze native DIAMOND inputs."""
import sys,json,time,shutil,math
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'mcp'))
from server import command
OUT=ROOT/'local/mk18';SNAP=OUT/'validated';SNAP.mkdir(parents=True,exist_ok=True)
def ready():
 until=time.monotonic()+60
 while time.monotonic()<until:
  s=command('state')
  if not s['busy']:
   assert not s['lastError'],s['lastError'];return s
  time.sleep(.025)
 raise AssertionError('Native source capture timed out')
def edit(method,**params):
 for attempt in range(5):
  try:return command(method,params,ready()['revision'])
  except RuntimeError as error:
   if 'Stale' not in str(error) or attempt==4:raise
def op(operation,**params):return edit('simulate',operation=operation,**params)
def export(kind,stem):
 result=edit('export',kind=kind);path=SNAP/(stem+'-'+kind+'.json');shutil.copyfile(result['path'],path);return json.loads(path.read_text())

def select(name,record):
 s=next(s for s in ready()['slots'] if s['name']==name);c=next(c for c in s['choices'] if c['record']==record)
 if s['selected']!=c['id']:edit('configure',slot=s['key'],mod=c['id']);ready()
 return c

def world(name):
 scene=command('pose');w=next(b['world'] for b in scene['bones'] if b['name']=='Weapon');p=next(p['world']['translate'] for p in scene['parts'] if p['name']==name)
 x,y,z,qw=w['rotation'];r=[[1-2*(y*y+z*z),2*(x*y-z*qw),2*(x*z+y*qw)],[2*(x*y+z*qw),1-2*(x*x+z*z),2*(y*z-x*qw)],[2*(x*z-y*qw),2*(y*z+x*qw),1-2*(x*x+y*y)]]
 delta=[a-b for a,b in zip(p,w['translate'])];return [sum(a*b for a,b in zip(col,delta))/w['scale'][0] for col in zip(*r)]

results={}
for family,title,expected in [('base','MK18',27),('empire','MK18-Empire',47)]:
 project=ROOT/'workspace'/f'{title}-native-reload.project.json'
 edit('open_project',path=str(project));ready();edit('workspace',index=5);ready();op('reset')
 select('Charging Handle','MK18.esp:004d9a');op('reset')
 slot=next(s for s in ready()['slots'] if s['name']=='Magazine');assert len(slot['choices'])==expected
 tested=[]
 for c in slot['choices']:
  op('reset');select('Magazine',c['record']);initial=op('reset');assert initial['active'] and not initial['error'] and not initial['magazine']['error'],initial
  stem=family+'-'+c['record'].replace(':','-');sim=export('simulation',stem);bundle=export('reload_bundle',stem)
  group=sim['authoredInteractions']['operatingGroups'];assert len(group)==1 and group[0]['hand']=='left'
  points=sim['authoredInteractions']['grabLocations'];assert len(points['points'])==2 and points['sourceName']=='WeaponExtra1'
  assert sim['magazineInteraction'].get('inputPath','straight')=='straight'
  bindings={b['hand']:b for b in bundle['magazineGrabBindings'] if b['ownerRecord']==c['record']};assert set(bindings)=={'left','right'}
  for hand in ['left','right']:
   edit('interaction_hand',hand=hand);op('reset');held=op('pull',progress=1);assert held['interaction']['applied']==(hand=='left');op('release');op('step',seconds=1);op('reset')
   op('mag_out');op('step',seconds=2);held=op('grab_magazine');assert held['magazineGrip']['applied'];rail=held['magazineRail']
   entered=op('move_magazine',position=[v*rail['travel']*.6 for v in rail['outwardInWeapon']]);assert entered['magazine']['phase']=='Full magazine on rail'
   op('move_magazine',distance=0);assert command('simulation_state')['magazinePresent']
  op('reset');rest=world('WeaponBolt');handle=world('WeaponExtra1');op('fire');op('step',seconds=.03)
  assert math.dist(world('WeaponBolt'),rest)>.1,'Firing bolt stayed static'
  assert math.dist(world('WeaponExtra1'),handle)<.005,'Nonreciprocating handle moved during fire'
  op('step',seconds=2);op('reset');op('trigger_down');empty=op('step',seconds=15);op('trigger_up')
  assert empty['ammunition']['shotsFired']==initial['ammunition']['capacity'] and empty['ammunition']['slideLocked'],empty['ammunition']
  op('drop_magazine');held=op('grab_magazine');rail=held['magazineRail'];op('move_magazine',position=[v*rail['travel']*.5 for v in rail['outwardInWeapon']]);op('move_magazine',distance=0);op('bolt_release');loaded=op('step',seconds=1);assert loaded['ammunition']['canFire']
  op('reset');op('fire');op('step',seconds=1);op('reload');loaded=op('step',seconds=12);assert loaded['ammunition']['canFire']
  assert not command('sound_state')['error'],command('sound_state')['error']
  tested.append({'record':c['record'],'model':c['model'],'capacity':initial['ammunition']['capacity']})
  print('PASS magazine',family,c['record'],flush=True)
  (OUT/(family+'-validated.json')).write_text(json.dumps(tested,indent=2))
 select('Magazine','MK18.esp:000fb9');handles=next(s for s in ready()['slots'] if s['name']=='Charging Handle')
 for c in handles['choices']:
  op('reset');select('Charging Handle',c['record']);edit('interaction_hand',hand='left');op('reset')
  for u in [0,.25,.5,.75,1]:assert op('pull',progress=u)['interaction']['applied']
  op('release');op('step',seconds=1);op('reset');export('simulation',family+'-handle-'+c['record'].replace(':','-'))
  print('PASS handle',family,c['record'],flush=True)
 select('Charging Handle','MK18.esp:004d9a');receivers=next(s for s in ready()['slots'] if s['name']=='Receiver')
 for c in receivers['choices']:
  op('reset');select('Receiver',c['record']);initial=op('reset');assert initial['active'] and not initial['error'],initial
  op('fire');after=op('step',seconds=2);assert after['ammunition']['shotsFired']==1
  print('PASS receiver',family,c['record'],flush=True)
 select('Receiver','MK18.esp:000fbe');op('reset');edit('interaction_hand',hand='left');edit('save_project',path=str(project))
 results[family]={'magazines':tested,'handles':handles['choices'],'receivers':receivers['choices']}
 (OUT/'validated-catalog.json').write_text(json.dumps(results,indent=2))
print('PASS all MK18 sources, magazine hands, insertion, cycling, sounds and selectors',flush=True)
