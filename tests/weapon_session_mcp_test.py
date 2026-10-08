"""Only reload, grab-point and scope data mark a weapon; where it was left is its session.

Opening the magazine grip editor changes nothing; a real grip edit marks the
weapon. Another magazine, a page and the camera never mark it, and they come
back when the weapon is opened again, from the weapon's session file. Export
everything exports the saved work of the loaded source's weapons, skipping
the others, into one archive.
"""
import json
from pathlib import Path
import sys
import time
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
sys.path.insert(0, str(ROOT/'tools'))
from template_mcp_test import ready, edit
from server import command
import product_library as library

EXPORT_TIMEOUT_SECONDS = 1800


def grip_transform():
    path = command('export', {'kind': 'grip'}, ready()['revision'])['path']
    return json.loads(Path(path).read_text(encoding='utf-8'))['pose']['placement']['handInAnchor']


def magazine_slot(state):
    return next(s for s in state['slots'] if len(s['choices']) > 1 and 'mag' in s['name'].lower())


def run():
    edit('testbed', dict(name='ru556')); state = ready()
    assert state['modified'] is False, 'a freshly opened weapon is marked modified'

    # Opening an editor is not an edit; moving the hand is.
    opened = edit('configure_magazine', dict(gripId=''))
    state = ready()
    assert state['modified'] is False and not state['magazineEditor']['unsaved'], 'opening the magazine grip editor marked the weapon'
    transform = grip_transform(); transform['translate'][0] += .4
    edit('set_grip', dict(transform=transform, name=state['magazineEditor']['name']))
    state = ready()
    assert state['modified'] is True and state['magazineEditor']['unsaved'], 'a grip edit did not mark the weapon'
    edit('testbed', dict(name='glock')); ready()
    edit('testbed', dict(name='ru556')); state = ready()
    assert state['modified'] is False, 'leaving did not discard the grip edit'

    # Loadout and camera are the weapon's session (a testbed always opens in Operate).
    slot = magazine_slot(state)
    other = next(c['id'] for c in slot['choices'] if c['id'] != slot['selected'])
    edit('configure', dict(slot=slot['key'], mod=other)); state = ready()
    assert state['modified'] is False, 'another magazine marked the weapon'
    edit('workspace', dict(index=7)); edit('view', dict(view='top', zoom=2.5)); state = ready()
    assert state['modified'] is False, 'a page or the camera marked the weapon'
    edit('testbed', dict(name='glock')); ready()
    edit('testbed', dict(name='ru556')); state = ready()
    assert magazine_slot(state)['selected'] == other, 'the magazine chosen last did not come back'
    assert state['modified'] is False, 'the restored session marked the weapon'
    plugin = command('esp_project')['plugin']
    sessions = json.loads((ROOT/'local/sessions'/(library.plugin_key(plugin)+'.json')).read_text(encoding='utf-8'))['weapons']
    kept = next(v for v in sessions.values() if v.get('choices', {}).get(slot['key']) == other)
    assert 'camera' in kept and 'soundProfiles' not in kept, sorted(kept)
    edit('configure', dict(slot=slot['key'], mod=slot['selected'])); edit('workspace', dict(index=5)); ready()

    # Export everything: the loaded source's saved weapons, one archive.
    archive = ROOT/'local/tmp'/('everything-'+uuid.uuid4().hex+'.zip')
    try:
        job = edit('pack_job', dict(operation='export', scope='everything', archive=str(archive)))
        deadline = time.monotonic()+EXPORT_TIMEOUT_SECONDS
        while True:
            state = command('pack_job')
            if state.get('id') == job['id'] and state.get('phase') in ('complete', 'failed'):
                break
            assert time.monotonic() < deadline, 'Export everything did not finish'
            time.sleep(.5)
        assert state['phase'] == 'complete' and state['scope'] == 'everything', state.get('error', state)
        result = state['result']
        assert result['plugins'] and 'scopeProfiles' in result, result
        with zipfile.ZipFile(archive) as packed:
            names = packed.namelist()
        assert any(name.endswith('.esp') for name in names), names[:20]
        assert all(s['reason'] for s in result['skipped']), result['skipped']
    finally:
        archive.unlink(missing_ok=True)
    print('PASS weapon session: editors open clean, edits mark, loadout and camera restore from the session, '
          'export everything delivers the loaded source:', len(result['plugins']), 'plugins,', result['scopeProfiles'], 'scope profiles,',
          len(result['skipped']), 'skipped', flush=True)


if __name__ == '__main__':
    run()
