"""Run with two real surface-picked locations in the current project's editor.

Exercises the running native controller; keeps all test mutations in local/.
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
            return command(method, params, command('state')['revision'])
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


original = ready()['project']
baseline = points()
assert len(baseline['points']) >= 2, 'Pick two actual operating-part surfaces first'
edit('save_project', path=str(OUT / 'original.project.json'))
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
        assert interactions['version'] == 2 and interactions['grabLocations'] == record
    # Wrong-hand rules and deliberate empty configurations must disable acquisition.
    for point in points()['points']:
        op('edit', id=point['id'], handMask=1)
    edit('workspace', index=5)
    ready()
    edit('interaction_hand', hand='right')
    assert not command('state')['interaction']['canGrab']
    edit('interaction_hand', hand='left')
    assert command('state')['interaction']['canGrab']
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
