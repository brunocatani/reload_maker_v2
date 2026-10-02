"""Glock native-source and physical-interaction coverage in the running editor."""
import json
import math
import shutil
import sys
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp'))
from server import command
OUT=ROOT/'evidence/glock-variants/validated'
OUT.mkdir(parents=True,exist_ok=True)
coverage={'profiles':[],'equipment':[],'cycles':[]}

def edit(method,params):return command(method,params,command('state')['revision'])
def op(name,**params):return edit('simulate',dict(operation=name,**params))
def ready():
    for _ in range(1200):
        s=command('state')
        if not s['busy']:
            assert not s['lastError'],s['lastError']
            return s
        time.sleep(.025)
    raise AssertionError('Assembly/capture timeout')
def finished():
    for _ in range(500):
        s=command('simulation_state')
        if s['action']=='Ready' and s['magazine']['phase'] not in ('Full magazine in','Empty magazine out','Loaded magazine out'):return s
        time.sleep(.02)
    raise AssertionError(('Action timeout',s))
def choose(slot,mod):
    current=next(x for x in command('state')['slots'] if x['key']==slot)
    if current['selected']!=str(mod):edit('configure',{'slot':slot,'mod':str(mod)});ready()
def count(s,mag,chamber,locked):
    a=s['ammunition'];assert (a['magazineRounds'],a['chambered'],a['slideLocked'])==(mag,chamber,locked),a
def valid():
    s=command('simulation_state')
    assert s['active'] and not s['error'] and not s['magazine']['error'],s
    r=s['magazineRail'];assert r['magazineMinProjection']+r['travel']>r['weaponMaxProjection']
    assert r['travel']>0 and math.isfinite(r['travel'])
    assert s['reloadProfile']['feedSeconds']>s['reloadProfile']['rackSeconds']>=0
    return s
def capture(name,view='side'):
    path=command('render',{'view':view,'fit':True,'preserveCamera':True})['path']
    shutil.copyfile(path,OUT/(name+'.png'))
    (OUT/(name+'.json')).write_text(json.dumps(command('simulation_state'),indent=2),encoding='utf-8')
def save_coverage():
    (OUT/'coverage.json').write_text(json.dumps(coverage,indent=2),encoding='utf-8')
def rack_empty():
    # Repeated full manual racks eject each live round; final release permits
    # the seated empty follower to engage the catch. No counter edit shortcut.
    for _ in range(70):
        s=command('simulation_state')
        if s['ammunition']['slideLocked']:return s
        op('pull',progress=1)
        if command('simulation_state')['ammunition']['magazineRounds']:
            op('pull',progress=0)
        op('release');finished()
    raise AssertionError('Rack/eject did not empty the weapon')
def full_cycle(key):
    op('reset');s=valid();capacity=s['ammunition']['capacity']
    native=op('reload');assert native['reloadProfile']['startedLoaded']
    ended=finished();count(ended,capacity-1,True,False)
    assert ended['ammunition']['roundsEjected']==1 and ended['reloadProfile']['cycled'] and ended['reloadProfile']['fed']
    op('reset')
    for n in range(capacity):
        fired=op('fire');assert fired['action']=='Firing'
        ended=finished();assert ended['ammunition']['shotsFired']==n+1
    count(ended,0,False,True);capture(key+'-empty-locked')
    op('mag_out');out=finished();count(out,0,False,True)
    visible=[x for x in out['magazineShapes'] if not x['flags']&1]
    if out['magazineRail']['sharedBody']:
        assert len(visible)==1,'Shared magazine body must remain while its cartridge is hidden'
    else:
        expected=[x for x in out['magazineShapes'] if x['emptyVersion'] and not x['originalFlags']&1]
        assert visible==expected and visible,'Empty magazine must retain all body/extender surfaces and hide the loaded branch'
    capture(key+'-empty-out','iso')
    op('grab_magazine');rail=valid()['magazineRail'];entry=[v*rail['travel'] for v in rail['outwardInWeapon']]
    op('move_magazine',position=[entry[0]+rail['captureRadius']*5,entry[1],entry[2]])
    op('move_magazine',position=entry);op('move_magazine',distance=0)
    count(valid(),capacity,False,True);op('slide_release');count(finished(),capacity-1,True,False)
    op('pull',progress=.5);op('release',cancel=True);count(valid(),capacity-1,True,False)
    rack_empty();op('drop_magazine');op('mag_in');count(finished(),capacity,False,True)
    op('pull',progress=1.08);op('release');count(finished(),capacity-1,True,False)
    op('drop_magazine');op('fire');count(finished(),0,False,False)
    op('mag_in');count(finished(),capacity,False,False)
    op('pull',progress=1);op('release');count(finished(),capacity-1,True,False)
    rack_empty();s=op('reload');assert s['reloadProfile']['startedOpen'] and not s['reloadProfile']['startedLoaded']
    assert s['parts'][0]['world']['translate'][1]<-2,'Empty reload snapped the locked slide closed'
    count(finished(),capacity-1,True,False)
    coverage['cycles'].append({'key':key,'capacity':capacity,'profile':valid()['reloadProfile']});save_coverage()

initial=ready();original_project=initial['project'];snapshot=OUT/'original-session.project.json'
edit('save_project',{'path':str(snapshot)})
slots={x['name']:x for x in initial['slots']}
mag=slots['Magazine']['key'];scope=slots['Sights']['key'];receiver=slots['Receiver']['key']
magazines=[0x1000fa4,0x1000fa5,0x100176f,0x1001770,0x1001779,0x100177a,0x1002e6c]
# Resolve representatives from current record metadata, not translated labels.
receivers=[next(x['id'] for x in slots['Receiver']['choices'] if x['editorId']==name) for name in
           ('mod_Glock19x_Receiver_Standard','mod_Glock19x_Receiver_Agency_Standard','mod_Glock19x_Receiver_Automatic1','mod_Glock19x_Receiver_Agency_Automatic1')]
try:
    edit('workspace',{'index':5});ready();op('spring',enabled=True)
    choose(receiver,receivers[0])
    # Use the simple grip/muzzle/accessory for source analysis; the user's
    # complete original configuration is restored from the saved snapshot.
    for label,edid in [('Barrel','mod_Glock19x_Barrel_Short'),('Grip','mod_Glock19x_Grip_Standard'),('Muzzle','mod_Glock19x_Muzzle')]:
        choice=next(x for x in slots[label]['choices'] if x['editorId']==edid);choose(slots[label]['key'],choice['id'])
    for slot in initial['slots']:
        none=next((x for x in slot['choices'] if x['editorId']=='mod_Glock19x_LowerRail_None'),None)
        if none:choose(slot['key'],none['id'])
    seen=set()
    assert command('state')['animationSource']['mode']=='omod-keywords'
    # Every physically distinct slide, firing mode, magazine and sight combination.
    for rid in receivers:
        choose(receiver,rid)
        for mid in magazines:
            choose(mag,mid)
            for sight in slots['Sights']['choices']:
                choose(scope,sight['id']);s=valid();op('reset')
                source=command('state')['animationSource'];recipe=command('reload')
                signature=recipe['source']['animationSha256']
                if signature not in seen:
                    full_cycle(signature[:12]);seen.add(signature)
                    coverage['profiles'].append({'identity':source,'source':recipe['source']['animation']})
                op('reset')
                before=command('diagnostics')['attachmentRemaps'][0]
                op('pull',progress=.7);pulled=command('diagnostics')['attachmentRemaps'][0]
                assert math.dist(before['world']['translate'],pulled['world']['translate'])>1
                assert math.dist(pulled['world']['translate'],pulled['parentWorld']['translate'])<.001
                op('release',cancel=True);op('fire');current=command('state')
                assert ('fireauto' in current['animation'])==s['ammunition']['automatic']
                finished();coverage['equipment'].append({'receiver':rid,'magazine':mid,'sights':sight['id'],'source':s['reloadProfile']['animation'],'automatic':s['ammunition']['automatic'],'capacity':s['ammunition']['capacity']})
            print('EQUIPMENT',rid,hex(mid),'PASS',flush=True);save_coverage()
    choose(receiver,receivers[0]);choose(mag,magazines[0]);choose(scope,0x1000fb1)
    exact=command('state')['animationSource']
    persisted=OUT/'omod-source.project.json';edit('save_project',{'path':str(persisted)});edit('open_project',{'path':str(persisted)});ready()
    assert command('state')['animationSource']==exact,'Reopened project changed the OMOD-resolved animation source'
    coverage['persistence']={'animationSource':exact};save_coverage()
    assert len(coverage['equipment'])==140 and coverage['cycles']
    print('PASS Glock: OMOD-resolved reload cycles, 140 geometry/firing combinations and source persistence',flush=True)

finally:
    edit('open_project',{'path':str(snapshot)});ready()
    if original_project:edit('save_project',{'path':original_project})
