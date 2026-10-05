"""Exercise the native RU556 testbed and preserve both weapon projects."""
import json
import math
import sys
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp'))
from server import command
OUT=ROOT/'evidence/ru556';OUT.mkdir(parents=True,exist_ok=True)

def edit(method,**params):return command(method,params,command('revision')['revision'])
def op(operation,**params):return edit('simulate',operation=operation,**params)
def ready():
    end=time.monotonic()+45
    while time.monotonic()<end:
        s=command('state')
        if not s['busy']:
            assert not s['lastError'],s['lastError']
            return s
        time.sleep(.03)
    raise AssertionError('Source did not become ready')
def sim():
    s=command('simulation_state')
    assert s['active'] and not s['error'] and not s['magazine']['error'],s
    return s
def select(slot,mod):
    current=next(s for s in command('state')['slots'] if s['key']==slot)
    if current['selected']!=mod:edit('configure',slot=slot,mod=mod);ready()
def matrix(q):
    x,y,z,w=q
    return [[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
            [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
            [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]]
def in_weapon(name):
    p=command('pose');weapon=next(b['world'] for b in p['bones'] if b['name']=='Weapon')
    part=next(b['world'] for b in p['parts'] if b['name']==name)
    inverse=list(zip(*matrix(weapon['rotation'])));delta=[a-b for a,b in zip(part['translate'],weapon['translate'])]
    return [sum(a*b for a,b in zip(row,delta)) for row in inverse]
def snapshot(label):
    r=command('render',{'view':'iso','fit':True,'preserveCamera':True})
    (OUT/(label+'.png')).write_bytes(Path(r['path']).read_bytes())

initial=ready();original=initial['project'];before=OUT/'original.project.json'
edit('save_project',path=str(before))
results={}
try:
    edit('testbed',name='glock');glock=ready();glock_grips=glock['magazineGrabGrips']
    assert sim()['ammunition']['resolved']
    edit('testbed',name='ru556');ru=ready();assert sim()['ar15']['boltSamples']>3
    original_choices={s['key']:s['selected'] for s in ru['slots']}
    magazine=next(s for s in ru['slots'] if s['name']=='Magazine')
    receiver=next(s for s in ru['slots'] if s['name']=='Receiver')
    automatic=next(c for c in receiver['choices'] if c['name']=='[Receiver] Automatic')
    select(receiver['key'],automatic['id'])
    capacities={'001ed3':20,'0089e3':20,'002684':30,'0089eb':30,'00267e':30,'0089e8':40,'004cb9':50,'0089ee':60}
    results['magazines']=[]
    for choice in magazine['choices']:
        select(magazine['key'],choice['id']);start=op('reset')
        capacity=capacities[choice['record'].split(':')[1]]
        assert start['ammunition']['capacity']==capacity and start['magazineRail']['sharedBody']
        bolt=in_weapon('WeaponBolt');handle=in_weapon('WeaponExtra1')
        op('trigger_down');op('step',seconds=.035)
        assert math.dist(bolt,in_weapon('WeaponBolt'))>.1,'Missing automatic bolt animation'
        assert math.dist(handle,in_weapon('WeaponExtra1'))<.004,'Charging handle reciprocated during automatic fire'
        emptied=op('step',seconds=capacity*.14+.25)
        assert emptied['ammunition']['shotsFired']==capacity and emptied['ar15']['boltLocked']
        op('trigger_up') # A later trigger_down must be a fresh physical press, including an unchanged receiver.
        assert emptied['ar15']['handle']==0 and not emptied['ammunition']['chambered']
        for shape in emptied['magazineShapes']:
            if shape['cartridge']:assert shape['flags']&1,'Empty magazine still shows a separate cartridge'
        locked_bolt=in_weapon('WeaponBolt')
        op('pull',progress=.45);op('release');partial=op('step',seconds=.5)
        assert partial['ar15']['boltLocked'] and partial['ar15']['handle']==0
        assert math.dist(locked_bolt,in_weapon('WeaponBolt'))<.004
        out=op('mag_out');out=op('step',seconds=.7)
        assert out['magazine']['phase']=='Empty magazine clear'
        dropped=op('drop_magazine')
        assert all(x['flags']&1 for x in dropped['magazineRoots'])
        assert math.dist(locked_bolt,in_weapon('WeaponBolt'))<.004,'Drop moved the receiver bolt'
        grabbed=op('grab_magazine')
        assert grabbed['magazineGrip']['applied'],grabbed['magazineGrip']
        if choice['record'].endswith(('001ed3','004cb9')):snapshot(choice['record'].split(':')[1]+'-held')
        rail=grabbed['magazineRail'];op('move_magazine',position=[x*rail['travel']*.5 for x in rail['outwardInWeapon']])
        seated=op('move_magazine',distance=0)
        assert seated['magazinePresent'] and seated['ar15']['boltLocked'] and not seated['ammunition']['chambered']
        op('bolt_release');fed=op('step',seconds=.5)
        assert fed['ammunition']['chambered'] and fed['ammunition']['magazineRounds']==capacity-1 and fed['ar15']['handle']==0
        op('pull',progress=1);assert sim()['ammunition']['roundsEjected']==1
        op('release',cancel=True);assert sim()['ammunition']['roundsEjected']==0
        op('pull',progress=1);op('release');racked=op('step',seconds=.5)
        assert racked['ammunition']['roundsEjected']==1 and racked['ammunition']['magazineRounds']==capacity-2
        op('reload');reloaded=op('step',seconds=10)
        assert reloaded['ammunition']['canFire'] and reloaded['ammunition']['magazineRounds']==capacity-1
        results['magazines'].append({'record':choice['record'],'capacity':capacity,'grip':grabbed['magazineGrip']['id']})
        print('PASS magazine',choice['name'],flush=True)
    results['receivers']=[]
    for choice in receiver['choices']:
        select(receiver['key'],choice['id']);state=op('reset');automatic_mode=state['ammunition']['automatic']
        op('trigger_down');fired=op('step',seconds=.31);op('trigger_up');ended=op('step',seconds=1)
        assert ended['ammunition']['shotsFired']>=2 if automatic_mode else ended['ammunition']['shotsFired']==1
        results['receivers'].append({'record':choice['record'],'automatic':automatic_mode})
    for key,value in original_choices.items():select(key,value)
    edit('save_project',path=str(ROOT/'workspace/RU556-native-reload.project.json'))
    exported=edit('export',kind='simulation');payload=json.loads(Path(exported['path']).read_text(encoding='utf-8'))
    assert payload['format']=='reload-maker-ar15-simulation' and payload['boltMotion']['poses']
    results['export']=exported['path']
    edit('testbed',name='glock');ready();assert command('state')['magazineGrabGrips']==glock_grips
    old=op('reset');old_bolt=in_weapon('WeaponBolt');op('pull',progress=1)
    assert math.dist(old_bolt,in_weapon('WeaponBolt'))>1,'Glock slide regressed'
    op('release');op('step',seconds=1)
    assert sim()['ammunition']['chambered']
    results['glock_preserved']=True
    (OUT/'validated.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    print('PASS all RU556 magazines, receiver modes, bolt/handle independence, physical insertion, native fits, export and Glock preservation')
finally:
    edit('open_project',path=str(before));ready();edit('save_project',path=original)
