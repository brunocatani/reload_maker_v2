"""Magazine editor regression against the running native application.

Uses a separate project, preserves all pre-existing library files, and restores
the caller's session. No game data or runtime deployment is involved.
"""
import copy
import json
import math
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'mcp'))
from server import command

OUT = ROOT / 'evidence/magazine-workflow'
OUT.mkdir(parents=True, exist_ok=True)
created = set()

def edit(method, **params):
    return command(method, params, command('revision')['revision'])

def ready():
    end = time.monotonic() + 40
    while time.monotonic() < end:
        state = command('state')
        if not state['busy']:
            assert not state['lastError'], state['lastError']
            return state
        time.sleep(.04)
    raise AssertionError('Editor did not finish the configuration')

def record(kind):
    result = edit('export', kind=kind)
    return json.loads(Path(result['path']).read_text(encoding='utf-8'))

def nearly(a, b, epsilon=3e-5):
    if isinstance(a, dict):
        assert set(a) == set(b), (a, b)
        for k in a: nearly(a[k], b[k], epsilon)
    elif isinstance(a, list):
        assert len(a) == len(b)
        for x, y in zip(a, b): nearly(x, y, epsilon)
    elif isinstance(a, (int, float)):
        assert abs(a-b) <= epsilon, (a, b)
    else:
        assert a == b, (a, b)

def select(mod):
    edit('workspace', index=5); ready()
    slot = next(x for x in command('state')['slots'] if x['name'] == 'Magazine')
    if slot['selected'] != str(mod): edit('configure', slot=slot['key'], mod=str(mod))
    return ready()

def save(scope='magazine'):
    before = set((ROOT/'workspace/part-grips').glob('*.json'))
    try: return edit('use_magazine_grip', scope=scope)
    finally: created.update(set((ROOT/'workspace/part-grips').glob('*.json')) - before)

initial = ready()
snapshot = OUT/'original.project.json'
working = OUT/'test.project.json'
originals = {p: p.read_bytes()
             for folder in ['hand-poses', 'part-grips']
             for p in (ROOT/'workspace'/folder).glob('*.json')}
edit('save_project', path=str(snapshot))
data = json.loads(snapshot.read_text(encoding='utf-8'))
data['workspace'] = 5
data.pop('gripDraft', None)
data['magazineGripDrafts'] = {}
working.write_text(json.dumps(data), encoding='utf-8')
results = {}
try:
    edit('open_project', path=str(working)); ready()
    select(0x1000fa4)
    before = command('state')['magazineGrabGrips']
    old_id = command('state')['magazineEditor']['gripId']
    source = json.loads((ROOT/'workspace/part-grips'/f'{old_id}.json').read_text(encoding='utf-8'))
    opened = edit('configure_magazine')
    assert opened['magazineEditor']['preview'] and opened['magazineEditor']['canSave']
    assert not opened['magazineEditor']['unsaved']
    nearly(record('grip')['pose']['placement']['handInAnchor'], source['pose']['placement']['handInAnchor'])
    before_pose = command('pose')
    transform = copy.deepcopy(source['pose']['placement']['handInAnchor'])
    far = copy.deepcopy(transform);far['translate'][2] -= 100
    edit('set_grip',transform=far,name='Reach preview regression')
    assert command('state')['magazineEditor']['canSave'], 'Authoring was blocked by fixed shoulder reach'
    far_pose=command('pose');nearly(before_pose['parts'],far_pose['parts'])
    for start,end in [('LArm_UpperArm','LArm_ForeArm1'),('LArm_ForeArm1','LArm_Hand')]:
        def length(p):
            bones={b['name']:b['world']['translate'] for b in p['bones']}
            return math.dist(bones[start],bones[end])
        assert abs(length(before_pose)-length(far_pose))<.001, 'Magazine preview stretched an arm segment'
    results['reachable_without_stretching']=True
    transform['translate'][0] += .4
    edit('set_grip', transform=transform, name='Workflow regression')
    after_pose = command('pose')
    nearly(before_pose['parts'], after_pose['parts'])
    for name in ['Weapon', 'RArm_Hand']:
        nearly(next(b['world'] for b in before_pose['bones'] if b['name']==name),
               next(b['world'] for b in after_pose['bones'] if b['name']==name))
    saved = save(); new_id = saved["magazineEditor"]["gripId"]
    assert new_id != old_id and not saved['magazineEditor']['unsaved'], saved['magazineEditor']
    assert len(saved['magazineGrabGrips']) == len(before)
    edit('save_project', path=str(working))  # nothing writes the project except Save (owner, 2026-10-05)
    persisted = json.loads(working.read_text(encoding='utf-8'))
    # Fits are assigned per hand; no key without a hand survives a save.
    key = next(k for k, v in before.items() if v == old_id)
    base = key.rsplit('|', 1)[0] if key.endswith(('|left', '|right')) else key
    assert persisted['magazineGrabGrips'][base + '|' + saved['magazineEditor']['hand']] == new_id
    assert base not in persisted['magazineGrabGrips'], 'Superseded legacy assignment kept'
    edit('workspace', index=5); ready()
    edit('simulate', operation='drop_magazine');edit('simulate', operation='grab_magazine')
    assert command('simulation_state')['magazineGrip']['id'] == new_id
    edit('configure_magazine'); nearly(record('grip')['pose']['placement']['handInAnchor'], transform)
    results['replacement_and_live_grab'] = True

    # Save a default; another magazine's explicit fit must remain unchanged.
    save('weapon_default')
    custom_33 = before[next(k for k in before if k.split('|')[1].endswith(':001779'))]
    select(0x1001779)
    assert command('state')['magazineEditor']['gripId'] == custom_33
    edit('configure_magazine'); edit('use_default_magazine_grip')
    inherited = command('state')['magazineEditor']
    assert inherited['assignment'] == 'default' and inherited['canSave']
    bundle = record('reload_bundle')
    assert any(g['pose']['id'] == inherited['gripId'] for g in bundle['grips'])
    assert any(b['gripId'] == inherited['gripId'] for b in bundle['magazineGrabBindings'])
    results['default_override_export'] = True

    # A drum with no custom fit starts from the explicit weapon default, then
    # a named Finger pose must supply all joints and the captured wrist angle.
    select(0x1002e6c)
    edit('configure_magazine')
    pose = next(p for p in command('state')['handPoses'] if p['name']=='Mag hold' and p['hand']=='left')
    finger_pose = json.loads((ROOT/'workspace/hand-poses'/f"{pose['id']}.json").read_text(encoding='utf-8'))
    prior_position = record('grip')['pose']['placement']['handInAnchor']['translate']
    edit('apply_magazine_pose', poseId=pose['id'])
    draft = record('grip')
    nearly(draft['pose']['fingerLocalTransforms'], finger_pose['fingerLocalTransforms'])
    nearly(draft['pose']['placement']['handInAnchor']['rotate'], finger_pose['placement']['handInAnchor']['rotate'])
    nearly(draft['pose']['placement']['handInAnchor']['translate'], prior_position)
    drum = save()
    assert drum['magazineEditor']['assignment'] == 'custom' and drum['magazineEditor']['canSave']
    drum_id = drum['magazineEditor']['gripId']
    edit('save_project', path=str(working))  # nothing writes the project except Save (owner, 2026-10-05)
    edit('open_project', path=str(working)); ready()
    edit('workspace', index=5); ready();edit('configure_magazine')
    assert command('state')['magazineEditor']['gripId'] == drum_id
    nearly(record('grip')['pose']['placement']['handInAnchor'], draft['pose']['placement']['handInAnchor'])
    results['drum_named_pose_rotation_reopen'] = True

    # An unsaved adjustment survives selecting a different magazine and back.
    transform = draft['pose']['placement']['handInAnchor']
    transform['translate'][2] += .3
    edit('set_grip', transform=transform, name='Draft survives navigation')
    select(0x1000fa4); select(0x1002e6c)
    edit('configure_magazine')
    assert command('state')['magazineEditor']['unsaved']
    nearly(record('grip')['pose']['placement']['handInAnchor'], transform)
    results['draft_navigation'] = True

    # Unconfigured magazine without any default offers the named pose library.
    blank = json.loads(working.read_text(encoding='utf-8'))
    blank['workspace']=5;blank.pop('gripDraft',None);blank['magazineGripDrafts']={}
    blank['magazineGrabGrips']={};blank['magazineGrabDefaults']={}
    empty = OUT/'unconfigured.project.json';empty.write_text(json.dumps(blank),encoding='utf-8')
    edit('open_project',path=str(empty));ready();edit('configure_magazine')
    assert command('state')['magazineEditor']['assignment']=='none'
    edit('apply_magazine_pose',poseId=pose['id'])
    assert command('state')['magazineEditor']['canSave']
    nearly(record('grip')['pose']['placement']['handInAnchor']['rotate'],finger_pose['placement']['handInAnchor']['rotate'])
    save();results['new_drum_from_named_pose']=True
    (OUT/'validated.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    print('PASS', ', '.join(results))
finally:
    edit('open_project',path=str(snapshot));ready()
    edit('save_project',path=initial['project'])
    for path,content in originals.items():
        assert path.read_bytes()==content, f'Original library changed: {path}'
    # Only remove the exact test-owned UUID files; never touch user records.
    for path in created:
        assert path.parent.resolve()==(ROOT/'workspace/part-grips').resolve() and path not in originals
        path.unlink()
