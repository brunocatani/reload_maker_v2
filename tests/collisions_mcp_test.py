"""Exercise collision authoring against the running native editor and Glock source.

Run manually with the editor open. Keeps exports in local/collision-research and
restores the original project. No game assets or providers are changed.
"""
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'mcp'))
from server import command

OUT = ROOT / 'local/collision-research'
OUT.mkdir(parents=True, exist_ok=True)
DIAMOND = ROOT.parents[1] / 'PROJECT_ROCK_V2/main_projects/DIAMOND'


def edit(method, **params):
    for attempt in range(5):
        try:
            return command(method, params, command('state')['revision'])
        except RuntimeError as error:
            if 'Stale document revision' not in str(error) or attempt == 4:
                raise


def ready(method='state'):
    until = time.monotonic() + 60
    while time.monotonic() < until:
        state = command(method)
        if not state['busy']:
            assert not state.get('error', state.get('lastError')), state
            return state
        time.sleep(.05)
    raise AssertionError('Native operation did not finish')


def collision(operation, **params):
    return edit('collisions', operation=operation, **params)


original = ready()['project']
original_workspace = command('state')['workspace']
original_camera = command('state')['camera']
results = {}
try:
    # Store any current authoring data before the normal testbed switch.
    edit('save_project', path=original)
    edit('testbed', name='glock')
    canonical = ready()['project']
    edit('workspace', index=7)
    ready()
    before_pose = command('pose')
    collision('defaults', target=0)
    whole = ready('collision_state')
    assert whole['valid'] and whole['hullCount'] > 1 and whole['inputTriangles'] > 0
    assert command('pose')['bones'] == before_pose['bones'], 'Collision generation changed pose'
    whole_path = OUT / 'glock-weapon.json'
    collision('export', path=str(whole_path))
    record = json.loads(whole_path.read_text(encoding='utf-8'))
    assert record['space'] == 'source-local' and record['units'] == 'game-units'
    assert not record['engineCooked'] and record['runtimeConsumerRequired']
    assert all(len(h['points']) <= 252 for h in record['hulls'])
    assert all(s['geometrySha256'] and len(s['sourceToWorld']) == 16 for s in record['sources'])
    assert not any('LArm' in s['name'] or 'RArm' in s['name'] for s in record['sources'])
    results['weapon'] = {k: whole[k] for k in ('hullCount', 'sourceCount', 'points', 'inputTriangles', 'warnings')}

    target = next(t['index'] for t in whole['targets'] if t['name'].startswith('Magazine '))
    collision('defaults', target=target)
    magazine = ready('collision_state')
    assert 0 < magazine['inputTriangles'] < whole['inputTriangles']
    path = OUT / 'glock-magazine.json'
    collision('hull', index=0, enabled=False)
    collision('export', path=str(path))
    collision('hull', index=0, enabled=True)
    collision('open', path=str(path))
    assert not command('collision_state')['hulls'][0]['enabled'], 'Saved hull exclusion lost'
    collision('hull', index=0, enabled=True)

    state = command('state')
    slot = next(s for s in state['slots'] if s['name'] == 'Magazine')
    selected = next(c for c in slot['choices'] if c['id'] == slot['selected'])
    variants = json.loads((DIAMOND / 'data/GlockMagazines.json').read_text(encoding='utf-8'))['magazines']
    variant = next(v for v in variants if v['record'].lower() == selected['record'].lower())
    source = DIAMOND / f"assets/collision/{variant['capacity']}-full.json"
    imported = collision('import', path=str(source))
    assert imported['origin'] == 'diamond-pre-elric-input' and imported['hullCount'] > 0
    imported_path = OUT / 'glock-imported-magazine.json'
    collision('export', path=str(imported_path))
    exported = json.loads(imported_path.read_text(encoding='utf-8'))
    legacy = json.loads(source.read_text(encoding='utf-8'))
    assert exported['settings']['massKg'] == legacy['mass']
    for a, b in zip(exported['hulls'], legacy['hulls']):
        assert len(a['points']) == len(b['points'])
        for p, q in zip(a['points'], b['points']):
            assert max(abs(x-y) for x, y in zip(p, q)) < .00002, 'Source units/coordinates changed'
    collision('open', path=str(imported_path))
    before = command('collision_state')
    bad = dict(legacy, mass=-1)
    bad_path = OUT / 'invalid-collision.json'
    bad_path.write_text(json.dumps(bad), encoding='utf-8')
    try:
        collision('import', path=str(bad_path))
        raise AssertionError('Invalid collision accepted')
    except RuntimeError:
        assert command('collision_state')['hulls'] == before['hulls']

    collision('view', compare=True)
    images = [command('render', {'view': view, 'fit': True, 'preserveCamera': True})['path'] for view in ('side', 'iso')]
    results['magazine'] = {k: imported[k] for k in ('hullCount', 'sourceCount', 'points', 'warnings')}
    results['images'] = images
    other = next(c for c in slot['choices'] if c['id'] != slot['selected'])
    edit('configure', slot=slot['key'], mod=other['id'])
    ready()
    assert not command('collision_state')['valid'], 'Loadout rebuild retained stale collisions'
    try:
        collision('open', path=str(imported_path))
        raise AssertionError('Mismatched magazine profile accepted')
    except RuntimeError:
        pass
    edit('configure', slot=slot['key'], mod=slot['selected'])
    ready()
    # Restore the canonical testbed's authoring state without saving test mutations.
    edit('open_project', path=canonical)
    ready()
finally:
    edit('open_project', path=original)
    ready()
    edit('workspace', index=original_workspace)
    edit('view', camera=original_camera)

(OUT / 'native-results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
print(json.dumps(results, indent=2))
print('PASS native generation, pose isolation, magazine import, save/reload, invalid input, source change and rendering')
