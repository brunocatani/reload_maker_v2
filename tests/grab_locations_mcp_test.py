"""Two real surface-picked grab locations on the Glock's slide.

Opens the Glock testbed and snapshots it, then picks two points on the slide
itself, so the testbed's saved project never keeps them. Exercises the running
native controller; keeps all test mutations in local/.
"""
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'mcp'))
from server import command

OUT = ROOT / 'local/grab-points'
OUT.mkdir(parents=True, exist_ok=True)


def edit(method, **params):
    for attempt in range(5):
        try:
            return command(method, params, command('revision')['revision'])
        except RuntimeError as error:
            if 'Stale' not in str(error) or attempt == 4:
                raise


def ready():
    until = time.monotonic() + 60
    while time.monotonic() < until:
        state = command('state')
        if not state['busy']:
            assert not state['lastError'], state['lastError']
            return state
        time.sleep(.03)
    raise AssertionError('Native project load did not finish')


def points():
    return command('grab_locations_state')['profile']


def op(operation, **params):
    return edit('grab_locations', operation=operation, **params)


def exported(kind):
    return json.loads(Path(edit('export', kind=kind)['path']).read_text(encoding='utf-8'))


ready()
edit('testbed', name='glock')
original = ready()['project']
edit('save_project', path=str(OUT / 'original.project.json'))
op('open')
# The Receiver slot holds the slide (the operating part) and the frame; a pick
# on the frame is refused, so keep picking until two points stand.
size = command('part_at', {'x': 0, 'y': 0})
for fy in range(30, 71, 3):
    for fx in range(15, 86, 3):
        x, y = size['width'] * fx / 100, size['height'] * fy / 100
        if len(points()['points']) >= 2 or command('part_at', {'x': x, 'y': y}).get('label') != 'Receiver':
            continue
        try:
            op('pick', x=x, y=y)
        except RuntimeError:
            pass
baseline = points()
assert len(baseline['points']) >= 2, 'Could not pick two points on the slide'
baseline_pose = command('pose')
try:
    first = baseline['points'][0]
    op('edit', id=first['id'], name='Rear handle contact', radiusGame=1.25, handMask=1)
    changed = points()
    assert len(changed['points']) == len(baseline['points'])
    assert changed['points'][0]['id'] == first['id']
    assert changed['points'][0]['pointInSource'] == first['pointInSource']
    assert changed['points'][1:] == baseline['points'][1:]
    for invalid in ({'radiusGame': -1}, {'handMask': 0}, {'pointInSource': [1, '2', 3]}):
        try:
            op('edit', **invalid)
            raise AssertionError('Invalid point mutation accepted')
        except RuntimeError:
            assert points() == changed
    pose = command('pose')
    assert all(pose[key] == baseline_pose[key] for key in ('bones', 'parts', 'source'))
    saved = OUT / 'multiple.project.json'
    edit('save_project', path=str(saved))
    edit('open_project', path=str(saved))
    ready()
    assert points() == changed
    record = exported('grab_locations')
    assert all(record[key] == changed[key] for key in changed)
    edit('workspace', index=5)
    ready()
    for kind in ('simulation', 'reload_bundle'):
        interactions = exported(kind)['authoredInteractions']
        # Version 3 (2026-10-01) added operating fingers; grab locations are unchanged.
        assert interactions['version'] == 3 and interactions['grabLocations'] == record, (kind, interactions.get('version'))
    # Wrong-hand rules and deliberate empty configurations must disable acquisition.
    # The Glock's preset reloads with the left hand only, so the hand stays
    # left and the masks change instead: right-only points refuse it.
    for mask, grabbable in ((2, False), (1, True)):
        for point in points()['points']:
            op('edit', id=point['id'], handMask=mask)
        edit('workspace', index=5)
        ready()
        assert command('state')['interaction']['canGrab'] == grabbable, mask
    for point in list(points()['points']):
        op('remove', id=point['id'])
    assert not command('state')['interaction']['canGrab']
    assert command('grab_locations_state')['configured']
    assert exported('simulation')['authoredInteractions']['grabLocations']['points'] == []
    # Bad project import must leave the current source and configuration intact.
    invalid = json.loads(saved.read_text(encoding='utf-8'))
    next(iter(invalid['grabLocations'].values()))['points'][0]['radiusGame'] = -1
    bad_path = OUT / 'invalid.project.json'
    bad_path.write_text(json.dumps(invalid), encoding='utf-8')
    before = command('grab_locations_state')
    try:
        edit('open_project', path=str(bad_path))
        raise AssertionError('Invalid project accepted')
    except RuntimeError:
        after = command('grab_locations_state')
        assert after['profile'] == before['profile'] and after['configured'] == before['configured']
    (OUT / 'native-validation.json').write_text(json.dumps({
        'weapon': baseline['weapon'], 'source': baseline['sourcePath'],
        'realPickedPoints': baseline['points'],
        'checks': ['pose isolation', 'replacement', 'atomic rejection', 'save/reopen',
                   'simulation and bundle exports', 'hand masks', 'empty configuration', 'invalid project isolation']
    }, indent=2), encoding='utf-8')
    print('PASS native point editing, pose isolation, project persistence, exports and acquisition eligibility')
finally:
    edit('open_project', path=str(OUT / 'original.project.json'))
    ready()
    edit('save_project', path=original)
