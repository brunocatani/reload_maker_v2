"""Weapon-only Fit, zoom ordering, camera-preserving renders and project restoration."""
import json
import sys
import time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp'))
from server import command
OUT=ROOT/'evidence/glock-variants/camera';OUT.mkdir(parents=True,exist_ok=True)
def edit(method,params):return command(method,params,command('state')['revision'])
def ready():
    for _ in range(1000):
        s=command('state')
        if not s['busy']:
            assert not s['lastError'],s['lastError'];return s
        time.sleep(.03)
    raise AssertionError('Loading timeout')
initial=ready();snapshot=OUT/'original.project.json';edit('save_project',{'path':str(snapshot)})
try:
    before=command('state')['camera']
    command('render',{'view':'iso','fit':True,'zoom':1.2,'preserveCamera':True})
    assert command('state')['camera']==before,'Inspection capture changed the user camera'
    edit('view',{'view':'side','fit':True});baseline=command('state')['camera']
    assert 1<baseline['distance']<50 and max(abs(x) for x in baseline['position'])<40,'Parked reload geometry expanded the Glock bounds'
    navigation=command('diagnostics')['viewport']['navigationScale']
    assert 1<navigation<50,'Effects/arms expanded camera manipulation speed'
    command('render',{'view':'current','fit':True,'zoom':1.2})
    assert abs(command('state')['camera']['zoom']-1.2)<1e-5,'Deferred Fit reset requested zoom'
    command('render',{'view':'current'})
    assert abs(command('state')['camera']['zoom']-1.2)<1e-5,'Later paint reset zoom'
    edit('workspace',{'index':5});ready()
    edit('simulate',{'operation':'drop_magazine'});edit('simulate',{'operation':'grab_magazine'})
    edit('simulate',{'operation':'move_magazine','position':[400,300,500]})
    edit('view',{'view':'side','fit':True});detached=command('state')['camera']
    assert abs(detached['distance']-baseline['distance'])<.001
    assert max(abs(a-b) for a,b in zip(detached['position'],baseline['position']))<.001,'Loose magazine changed Fit reference'
    edit('simulate',{'operation':'cancel_magazine'});edit('simulate',{'operation':'reset'})
    edit('configure',{'body':'female' if initial['body']=='male' else 'male'});ready()
    edit('view',{'view':'side','fit':True});other_body=command('state')['camera']
    assert abs(other_body['distance']-baseline['distance'])<.01,'Arm/body geometry changed weapon Fit'
    assert abs(command('diagnostics')['viewport']['navigationScale']-navigation)<.01,'Reassembly changed navigation scale'
    edit('workspace',{'index':5});ready()
    reload_action=next(x['index'] for x in command('state')['actions'] if x['name']=='Reload')
    edit('action',{'index':reload_action})
    edit('playback',{'mode':1,'seconds':command('state')['duration']*.5})
    edit('view',{'view':'side','fit':True})
    assert 1<command('state')['camera']['distance']<50,'Native reload props expanded weapon Fit'
    edit('view',{'camera':before});project=OUT/'saved-camera.project.json';edit('save_project',{'path':str(project)})
    edit('view',{'view':'top','fit':True});edit('open_project',{'path':str(project)});ready()
    assert command('state')['camera']==before,'Project did not restore exact camera'
    (OUT/'validated.json').write_text(json.dumps({'baseline':baseline,'detached':detached,'otherBody':other_body,'restored':before},indent=2),encoding='utf-8')
    print('PASS Fit remains on weapon with distant loose magazine and alternate arms; zoom survives paint; inspection renders and project restore preserve camera')
finally:
    edit('open_project',{'path':str(snapshot)});ready();edit('save_project',{'path':initial['project']})
