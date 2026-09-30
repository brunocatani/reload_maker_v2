"""Native M1A testbed: source motion, all magazine/reload variants and receiver modes."""
import json
import math
import sys
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp'))
from server import command
OUT=ROOT/'evidence/m1a';OUT.mkdir(parents=True,exist_ok=True)

def edit(method,**params):return command(method,params,command('state')['revision'])
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
    assert s['active'] and not s['error'] and not s['magazine']['error'],s
    return s

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
    return point,rotation

def photo(name):
    r=command('render',{'view':'iso','fit':True,'preserveCamera':True})
    (OUT/(name+'.png')).write_bytes(Path(r['path']).read_bytes())

original=ready()['project'];backup=OUT/'original.project.json'
edit('save_project',path=str(backup));results={'magazines':[],'receivers':[]}
try:
    preserved={}
    for name in ['glock','ru556']:
        edit('testbed',name=name);s=ready();sim();preserved[name]=s['magazineGrabGrips']
    edit('testbed',name='m1a');s=ready()
    choices={a['key']:a['selected'] for a in s['slots']};initial_set=s['animationSet']
    magazine=next(a for a in s['slots'] if a['name']=='Magazine')
    receiver=next(a for a in s['slots'] if a['name']=='Receiver')
    sets=[a for a in s['animationSets'] if a['identity'].get('mode')=='explicit' and a['identity'].get('record')=='M1A.esp:001081']
    assert len(sets)==2
    for animation in sets:
        edit('configure',animationSet=animation['index']);ready()
        for choice in magazine['choices']:
            edit('configure',slot=magazine['key'],mod=choice['id']);ready();s=op('reset');sim()
            capacity=20 if 'Extended' in choice['name'] else 10
            assert s['ammunition']['capacity']==capacity and not s['ammunition']['automatic']
            assert s['magazineRail']['version']==2 and len(s['magazineRail']['guide'])>10
            closed=relative('WeaponBolt');handle=relative('WeaponExtra1');receiver_pose=relative('WeaponExtra3')
            op('trigger_down');op('step',seconds=.075)
            assert math.dist(closed[0],relative('WeaponBolt')[0])>1,'Missing firing bolt motion'
            assert math.dist(handle[0],relative('WeaponExtra1')[0])>1,'Operating handle did not reciprocate'
            op('step',seconds=1);assert sim()['ammunition']['shotsFired']==1,'Semi-auto held trigger repeated'
            for _ in range(capacity-1):op('fire');op('step',seconds=1)
            empty=sim();assert empty['m1a']['boltLocked'] and not empty['ammunition']['chambered']
            open_bolt=relative('WeaponBolt');open_handle=relative('WeaponExtra1')
            assert math.dist(handle[0],open_handle[0])>7 and math.dist(closed[0],open_bolt[0])>4
            assert max(abs(a-b) for ra,rb in zip(closed[1],open_bolt[1]) for a,b in zip(ra,rb))>.1,'Bolt rotation was flattened'
            for shape in empty['magazineShapes']:
                if shape['cartridge']:assert shape['flags']&1,'Empty magazine still shows cartridges'
                elif shape['name'].startswith('MagFollower:'):assert not shape['flags']&1,'Follower was hidden as a cartridge'
            op('mag_out');op('step',seconds=.7)
            dropped=op('drop_magazine');assert all(a['flags']&1 for a in dropped['magazineRoots'])
            assert math.dist(receiver_pose[0],relative('WeaponExtra3')[0])<.005,'Drop moved receiver geometry'
            grabbed=op('grab_magazine');assert grabbed['magazineGrip']['applied'],grabbed['magazineGrip']
            assert all(not a['flags']&1 for a in grabbed['magazineShapes'] if a['cartridge'])
            if animation==sets[0]:photo(choice['record'].split(':')[1]+'-held')
            rail=grabbed['magazineRail'];sample=min(rail['guide'],key=lambda p:abs(p['distance']-rail['travel']*.6))
            capture=op('move_magazine',position=sample['offset']);assert capture['magazine']['phase']=='Full magazine on rail'
            op('move_magazine',distance=sample['distance'])
            seated=op('move_magazine',distance=0)
            assert seated['magazinePresent'] and seated['m1a']['boltLocked'] and not seated['ammunition']['chambered']
            op('pull',progress=1.08);op('release',cancel=True);assert sim()['m1a']['boltLocked']
            op('pull',progress=1.08);op('release');fed=op('step',seconds=1)
            assert fed['ammunition']['chambered'] and fed['ammunition']['magazineRounds']==capacity-1
            op('pull',progress=1);assert sim()['ammunition']['roundsEjected']==1
            op('release');op('step',seconds=1)
            op('reload');loaded=op('step',seconds=10)
            assert loaded['ammunition']['canFire'] and loaded['ammunition']['magazineRounds']==capacity-1
            results['magazines'].append({'record':choice['record'],'animationSet':animation['name'],'capacity':capacity,'guideSamples':len(rail['guide'])})
            print('PASS magazine',choice['name'],'set',animation['index'],flush=True)
    for choice in receiver['choices']:
        edit('configure',slot=receiver['key'],mod=choice['id']);ready();s=op('reset')
        assert not s['ammunition']['automatic']
        op('trigger_down');s=op('step',seconds=3)
        assert s['ammunition']['shotsFired']==1 and s['ammunition']['canFire']
        results['receivers'].append(choice['record'])
    for key,value in choices.items():
        if next(a for a in command('state')['slots'] if a['key']==key)['selected']!=value:edit('configure',slot=key,mod=value);ready()
    edit('configure',animationSet=initial_set);ready()
    exported=edit('export',kind='simulation');payload=json.loads(Path(exported['path']).read_text(encoding='utf-8'))
    assert payload['format']=='reload-maker-m1a-simulation' and payload['reciprocatingHandle'] and payload['magazineInteraction']['version']==2
    bundled=edit('export',kind='reload_bundle');bundle=json.loads(Path(bundled['path']).read_text(encoding='utf-8'))
    assert len(bundle['magazineGrabBindings'])==4
    assert {g['pose']['hand'] for g in bundle['grips']}=={'right'}
    results['export']=exported['path'];results['bundle']=bundled['path']
    edit('save_project',path=str(ROOT/'workspace/M1A-native-reload.project.json'))
    for name in ['glock','ru556']:
        edit('testbed',name=name);s=ready();assert s['magazineGrabGrips']==preserved[name];sim()
    (OUT/'validated.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    print('PASS M1A normal/Quick reloads, all magazines/receivers, source bolt rotation and handle coupling, curved guide, payload states, native fits and exports')
finally:
    edit('open_project',path=str(backup));ready();edit('save_project',path=original)
