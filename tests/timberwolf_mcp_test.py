"""C14 native bolt cycle, configured ammunition, magazines and authored fits."""
import json
import math
import sys
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp'))
from server import command
OUT=ROOT/'evidence/timberwolf';OUT.mkdir(parents=True,exist_ok=True)

def edit(method,**params):
    for attempt in range(5):
        try:return command(method,params,command('revision')['revision'])
        except RuntimeError as error:
            if 'Stale document revision' not in str(error) or attempt==4:raise
            print('retrying after:',error,flush=True)
def op(operation,**params):return edit('simulate',operation=operation,**params)
def ready():
    deadline=time.monotonic()+45
    while time.monotonic()<deadline:
        s=command('state')
        if not s['busy']:
            assert not s['lastError'],s['lastError']
            return s
        time.sleep(.03)
    raise AssertionError('Source did not finish loading')
def sim():
    s=command('simulation_state')
    assert s['active'] and not s['error'] and not s['magazine']['error'],s['error'] or s['magazine']['error']
    return s
def select(slot,choice):
    if next(a for a in command('state')['slots'] if a['key']==slot['key'])['selected']!=choice['id']:
        edit('configure',slot=slot['key'],mod=choice['id']);ready()
def matrix(q):
    x,y,z,w=q
    return [[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
            [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
            [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]]
def relative(name):
    p=command('pose');w=next(b['world'] for b in p['bones'] if b['name']=='Weapon')
    t=next(b['world'] for b in p['parts'] if b['name']==name)
    inverse=list(zip(*matrix(w['rotation'])));delta=[a-b for a,b in zip(t['translate'],w['translate'])]
    point=[sum(a*b for a,b in zip(row,delta)) for row in inverse]
    rotation=[[sum(a*b for a,b in zip(row,col)) for col in zip(*matrix(t['rotation']))] for row in inverse]
    return point,[v for row in rotation for v in row]
def photo(name):
    r=command('render',{'view':'iso','fit':True,'preserveCamera':True})
    (OUT/(name+'.png')).write_bytes(Path(r['path']).read_bytes())

original=command('export',{'kind':'project'},ready()['revision'])['path'];backup=OUT/'original.project.json'
edit('export',kind='project',path=str(backup));results={'magazines':[],'receivers':[],'ammo':[]}
try:
    edit('testbed',name='timberwolf');s=ready();sim()
    slots={a['name']:a for a in s['slots']}
    for choice in slots['Magazine']['choices']:
        select(slots['Magazine'],choice);s=op('reset');sim()
        capacity=10 if 'Extended' in choice['name'] else 5
        assert s['ammunition']['capacity']==capacity and not s['ammunition']['automatic']
        assert not s['setup']['springReturn']
        closed=relative('WeaponBolt');pin=relative('WeaponOptics2')
        op('trigger_down');shot=op('step',seconds=3)
        assert shot['ammunition']['shotsFired']==1 and not shot['ammunition']['canFire']
        assert shot['boltAction']['spentCase'] and shot['ammunition']['magazineRounds']==capacity-1
        assert math.dist(closed[0],relative('WeaponBolt')[0])<.005,'Fire opened a manual bolt'
        assert math.dist(pin[0],relative('WeaponOptics2')[0])>.3,'Firing pin did not preserve its fired position'
        op('pull',progress=.4);op('release');held=op('step',seconds=3)
        assert abs(held['slide']-.4)<.001 and held['boltAction']['spentCase']
        assert max(abs(a-b) for a,b in zip(closed[1],relative('WeaponBolt')[1]))>.5,'Native bolt lift was flattened'
        threshold=held['boltAction']['ejectProgress'];op('pull',progress=(1+threshold)/2)
        assert sim()['boltAction']['casesEjected']==1 and not sim()['boltAction']['spentCase']
        op('release',cancel=True);s=sim();assert s['boltAction']['spentCase'] and s['boltAction']['casesEjected']==0
        op('pull',progress=(1+threshold)/2);op('pull',progress=0);op('release')
        assert not sim()['ammunition']['chambered'],'Short stroke fed without reaching rear'
        op('pull',progress=1);op('release');s=op('step',seconds=2)
        assert s['slide']==1 and s['boltAction']['closingPath'] and not s['ammunition']['slideLocked']
        assert math.dist(closed[0],relative('WeaponBolt')[0])>8
        photo(choice['record'].split(':')[1]+'-open')
        dropped=op('drop_magazine');assert all(a['flags']&1 for a in dropped['magazineRoots'])
        grabbed=op('grab_magazine');assert grabbed['magazineGrip']['applied'],grabbed['magazineGrip']
        for shape in grabbed['magazineShapes']:
            assert shape['flags']==shape['originalFlags'],'Fresh magazine changed native variant visibility'
        rail=grabbed['magazineRail'];assert rail['inputPath']=='straight' and rail['version']==3
        capture=op('move_magazine',position=[a*rail['travel']*.6 for a in rail['outwardInWeapon']])
        assert capture['magazine']['phase']=='Full magazine on rail'
        op('move_magazine',distance=rail['travel']*.3)
        seated=op('move_magazine',distance=0)
        assert seated['magazinePresent'] and seated['slide']==1 and not seated['ammunition']['chambered']
        op('pull',progress=.2);assert not sim()['ammunition']['canFire']
        op('pull',progress=0);op('release');s=sim()
        assert s['ammunition']['canFire'] and s['ammunition']['magazineRounds']==capacity-1
        assert math.dist(pin[0],relative('WeaponOptics2')[0])<.005,'Lock failed to cock firing pin'
        for n in range(capacity):
            op('fire');op('step',seconds=2);op('pull',progress=1);op('pull',progress=0);op('release')
        empty=sim();assert not empty['ammunition']['chambered'] and not empty['ammunition']['slideLocked']
        assert empty['boltAction']['casesEjected']==capacity+1
        for shape in empty['magazineShapes']:
            assert shape['flags']==(shape['originalFlags']|1 if shape['cartridge'] else shape['originalFlags']),shape
        op('mag_out');op('step',seconds=1);op('drop_magazine');op('grab_magazine')
        photo(choice['record'].split(':')[1]+'-held');op('cancel_magazine')
        op('pull',progress=1);op('pull',progress=0);op('release')
        assert not sim()['ammunition']['chambered'],'Absent magazine created a round'
        op('reset');op('fire');op('step',seconds=2);op('reload');loaded=op('step',seconds=10)
        assert loaded['ammunition']['canFire'] and not loaded['boltAction']['spentCase']
        assert loaded['ammunition']['magazineRounds']==capacity-1 and loaded['boltAction']['casesEjected']==1
        results['magazines'].append({'record':choice['record'],'animationSource':command('state')['animationSource'],'capacity':capacity})
        print('PASS magazine',choice['name'],flush=True)
    for choice in slots['Receiver']['choices']:
        select(slots['Receiver'],choice);s=op('reset');op('trigger_down');s=op('step',seconds=3)
        assert not s['ammunition']['automatic'] and s['ammunition']['shotsFired']==1 and s['boltAction']['spentCase'],(choice['name'],s['action'],s['ammunition'],s['boltAction'])
        results['receivers'].append(choice['record'])
    for choice in slots['Ammo']['choices']:
        select(slots['Ammo'],choice);s=op('reset');assert s['ammunition']['resolved'],s['ammunition']['error']
        results['ammo'].append({'record':choice['record'],'ammo':s['ammunition']['ammoRecord']})
    assert len({a['ammo'] for a in results['ammo']})==4
    exported=edit('export',kind='simulation');payload=json.loads(Path(exported['path']).read_text(encoding='utf-8'))
    assert payload['format']=='reload-maker-bolt-action-simulation' and not payload['springReturn']
    motion=payload['boltMotion'];assert len(motion['tracks'])==3 and motion['release']=='hold'
    for phase,closing in [('openingProgress',False),('closingProgress',True)]:
        op('reset');op('fire');op('step',seconds=2)
        if closing:op('pull',progress=1)
        start=motion['rearFrame'] if closing else motion['firstFrame']
        for offset in range(1,len(motion[phase])-1,4):
            progress=motion[phase][offset];op('pull',progress=1-progress if closing else progress)
            for i,name in enumerate(['WeaponBolt','WeaponExtra1','WeaponOptics2']):
                actual=relative(name);expected=motion['tracks'][i]['poses'][start+offset]
                assert math.dist(actual[0],expected['translate'])<.005,(phase,name,offset)
                assert max(abs(a-b) for a,b in zip(actual[1],expected['rotate']))<.005,(phase,name,offset)
        op('release')
    bundled=edit('export',kind='reload_bundle');bundle=json.loads(Path(bundled['path']).read_text(encoding='utf-8'))
    assert sum(b.get('hand', 'left')=='left' for b in bundle['magazineGrabBindings'])==2
    results.update(export=exported['path'],bundle=bundled['path'])
    (OUT/'validated.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    print('PASS native C14 lift/pull/push/lock, spent/live ejection, manual hold, short strokes, magazine insertion, all reload/ammo/receiver choices and exports')
finally:
    edit('import_project',path=str(backup));ready();edit('export',kind='project',path=original)
