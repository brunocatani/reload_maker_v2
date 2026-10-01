"""Validate native SREP assemblies, saved hand contact frames and full reload loops.

Freeze exact tested inputs for DIAMOND, including every receiver/magazine pair.
Requires the four authored SREP projects and the actual installed source package.
"""
import json,math,shutil,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'mcp'))
from server import command
OUT=ROOT/'local/srep';SNAP=OUT/'validated';SNAP.mkdir(parents=True,exist_ok=True)
EMBEDDED={'1CAS_ServiceRifle.esp:'+n for n in ('00c7ba','00c7bd','00c80c')}

def ready():
    for _ in range(2400):
        state=command('state')
        if not state['busy']:
            assert not state['lastError'],state['lastError'];return state
        time.sleep(.025)
    raise AssertionError('Source capture timed out')

def edit(method,**params):return command(method,params,ready()['revision'])
def op(operation,**params):return edit('simulate',operation=operation,**params)
def select(record):
    state=ready();slot=next(s for s in state['slots'] if any(c['record']==record for c in s['choices']))
    choice=next(c for c in slot['choices'] if c['record']==record)
    if slot['selected']!=choice['id']:edit('configure',slot=slot['key'],mod=choice['id']);ready()

def export(kind,stem):
    path=Path(edit('export',kind=kind)['path']);destination=SNAP/(stem+'-'+kind+'.json')
    shutil.copyfile(path,destination);return json.loads(destination.read_text())

def rotation(q):
    x,y,z,w=q
    return [[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
            [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
            [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]]

def relative(root,point):
    delta=[a-b for a,b in zip(point,root['translate'])]
    return [sum(a*b for a,b in zip(row,delta))/root['scale'][0] for row in zip(*rotation(root['rotation']))]

def check_hand(grip):
    scene=command('pose');p=grip['placement'];prefix='L' if grip['hand']=='left' else 'R'
    root=next(x['world'] for x in scene['parts'] if x['name']==p['anchorName'] and x['ownerRecord']==p['ownerRecord'])
    wrist=next(b['world'] for b in scene['bones'] if b['name']==prefix+'Arm_Hand')
    assert math.dist(relative(root,wrist['translate']),p['handInAnchor']['translate'])<.015
    actual=[sum(a*b for a,b in zip(row,col)) for row in zip(*rotation(root['rotation'])) for col in zip(*rotation(wrist['rotation']))]
    assert max(abs(a-b) for a,b in zip(actual,p['handInAnchor']['rotate']))<.004
    for expected in grip['fingerLocalTransforms']:
        actual=next(b['local'] for b in scene['bones'] if b['name']==expected['bone'])
        assert math.dist(actual['translate'],expected['translate'])<.015
        assert max(abs(a-b) for a,b in zip(sum(rotation(actual['rotation']),[]),expected['rotate']))<.004

def position(name):
    scene=command('pose');root=next(b['world'] for b in scene['bones'] if b['name']=='Weapon')
    return relative(root,next(p['world']['translate'] for p in scene['parts'] if p['name']==name))

def magazine_checks(record):
    initial=op('reset')
    for hand in ('left','right'):
        edit('interaction_hand',hand=hand);op('drop_magazine');held=op('grab_magazine')
        assert held['magazineGrip']['applied']
        grip=json.loads((ROOT/'workspace/part-grips'/(held['magazineGrip']['id']+'.json')).read_text())['pose'];check_hand(grip)
        rail=held['magazineRail'];entered=op('move_magazine',position=[v*rail['travel']*.8 for v in rail['outwardInWeapon']])
        assert entered['magazine']['phase']=='Full magazine on rail';op('move_magazine',distance=rail['travel']*.5);check_hand(grip)
        op('move_magazine',distance=0);assert command('simulation_state')['magazinePresent'];op('reset')
    edit('interaction_hand',hand='left');rest=position('WeaponBolt');op('fire');op('step',seconds=.035)
    assert math.dist(position('WeaponBolt'),rest)>.1,'The native firing bolt stayed static'
    op('step',seconds=2);op('reset')
    if initial['ammunition']['automatic']:
        op('trigger_down');empty=op('step',seconds=20);op('trigger_up')
    else:
        for _ in range(initial['ammunition']['capacity']):op('fire');empty=op('step',seconds=1)
    assert empty['ammunition']['slideLocked'] and not empty['ammunition']['canFire'],empty['ammunition']
    op('mag_out');op('step',seconds=2)
    assert all(v['flags']&1 for v in command('simulation_state')['magazineShapes'] if v['cartridge'])
    held=op('grab_magazine');rail=held['magazineRail'];op('move_magazine',position=[v*rail['travel']*.5 for v in rail['outwardInWeapon']]);op('move_magazine',distance=0)
    op('bolt_release');assert op('step',seconds=1)['ammunition']['canFire'];op('reset')
    op('fire');op('step',seconds=1);op('reload');assert op('step',seconds=12)['ammunition']['canFire'];op('reset')
    assert not command('sound_state')['error'],command('sound_state')['error']
    return initial['ammunition']['capacity']

def main():
    catalog=json.loads((OUT/'catalog.json').read_text()) if len(sys.argv)>1 else {}
    for family,weapon,default,expected in [('service','00213f','00c7ac',14),('honeybadger','005bdd',None,14),('ar10','00d875','00d8bc',3),('xm16e1','00d8a5','00d89b',2)]:
        if len(sys.argv)>1 and family not in sys.argv[1:]:continue
        project=ROOT/'workspace'/('srep-'+family+'-native-reload.project.json');edit('open_project',path=str(project));ready();edit('workspace',index=5);ready();op('reset')
        if default:select('1CAS_ServiceRifle.esp:'+default);op('reset')
        state=ready();magazines=next(s['choices'] for s in state['slots'] if s['name']=='Magazine');assert len(magazines)==expected
        receivers=next(s['choices'] for s in state['slots'] if s['name'].lower()=='receiver') if default else [{'record':'1CAS_ServiceRifle.esp:'+weapon,'name':'Fixed receiver'}]
        embedded=[r for r in receivers if r['record'] in EMBEDDED]
        summary={'weapon':'1CAS_ServiceRifle.esp:'+weapon,'receivers':receivers,'embedded':[r['record'] for r in embedded],
                 'mechanism':'reciprocating-bolt' if family=='ar10' else 'ar15','magazines':[],'variants':[]}
        tested=set()
        for receiver in receivers:
            if default:select(receiver['record']);op('reset')
            choices=[receiver] if receiver in embedded else magazines
            for mag in choices:
                if receiver not in embedded:select(mag['record'])
                initial=op('reset');assert initial['active'] and not initial['error'] and not initial['magazine']['error'],initial
                edit('interaction_hand',hand='left')
                for progress in (0,.5,1):assert not op('pull',progress=progress)['interaction']['applied']
                op('release',cancel=True);op('reset')
                stem=family+'-'+receiver['record'].split(':')[1]+'-'+mag['record'].split(':')[1]
                sim=export('simulation',stem);assert len(sim['authoredInteractions']['grabLocations']['points'])==2
                assert len(sim['slideTracks'])==2 # complete input / bolt catch, plus independent bolt below
                if summary['mechanism']=='ar15':
                    assert sim['manualBoltFromReload']
                    op('pull',progress=1)
                    expected_bolt=sim['sourceRecipe']['tracks'][sim['boltTrack']]['poses'][sim['openFrame']][:3]
                    assert math.dist(position('WeaponBolt'),expected_bolt)<.015,'Manual bolt used firing recoil instead of the native reload stroke'
                    op('release',cancel=True);op('reset')
                summary['variants'].append({'receiver':receiver['record'],'magazine':mag['record'],'stem':stem})
                if mag['record'] not in tested:
                    capacity=magazine_checks(mag['record']);tested.add(mag['record']);op('reset')
                    stem=family+'-'+mag['record'].replace(':','-');export('simulation',stem);export('reload_bundle',stem)
                    (SNAP/(stem+'-native-seat.json')).write_text(json.dumps(command('pose')))
                    summary['magazines'].append({**mag,'capacity':capacity})
                print('PASS',family,receiver['record'],mag['record'],flush=True)
        catalog[family]=summary;(OUT/'catalog.json').write_text(json.dumps(catalog,indent=2))
        if default:select('1CAS_ServiceRifle.esp:'+default)
        select(magazines[0]['record']);op('reset');edit('save_project',path=str(project))
    print('PASS',sum(len(c['variants']) for c in catalog.values()),'configurations; all magazine wrists, joints and reloads',flush=True)

if __name__=='__main__':main()
