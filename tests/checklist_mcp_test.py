"""Checklist confirmations follow authored data, persist, and ignore view-only changes.

Replaces the retired roadmap suites (2026-10-04): preset assignment and the
recorded source inputs, precise invalidation by the magazine guide and the
collision hulls, camera and overlay independence, project reopen, and an
authoring record the checklist creates by itself.
"""
import json
import sys
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'mcp'))
from server import command

def idle():
    for _ in range(24000):
        current = command('revision')
        if not current['busy']:
            assert not current['lastError'], current['lastError']
            return current['revision']
        time.sleep(.005)
    raise AssertionError('Editor did not finish loading or capturing')


def ready():
    idle()
    return command('state')


def edit(method, params):
    return command(method, params, idle())


def collisions_ready():
    for _ in range(2400):
        state = command('collision_state')
        if not state.get('busy', False):
            assert not state.get('error', ''), state
            return state
        time.sleep(.05)
    raise AssertionError('Collision generation did not finish')


def items():
    return {item['id']: item for item in command('checklist_state')['items']}


def states():
    return {key: item['state'] for key, item in items().items()}


def confirm_all():
    # Confirming one item can make another one confirmable.
    for _ in range(4):
        pending = [key for key, item in items().items() if item['canConfirm']]
        if not pending:
            return
        for key in pending:
            edit('checklist', dict(operation='confirm', id=key))
    raise AssertionError(('Items stayed confirmable', pending))


def records(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))['authoringRoadmaps']


def run():
    work = ROOT / 'local/checklist-tests' / uuid.uuid4().hex
    work.mkdir(parents=True)
    original = ready()['project']
    original_bytes = Path(original).read_bytes() if original else None
    try:
        edit('testbed', dict(name='glock'))
        project = Path(ready()['project'])
        saved = project.read_bytes()
        try:
            edit('workspace', dict(index=5))
            ready()
            state = command('checklist_state')
            assert state['evaluated'], state['problem']
            assert {preset['id'] for preset in state['presets']} >= {state['preset']}, state['presets']
            edit('checklist', dict(operation='preset', id=state['preset']))
            ready()
            assert command('checklist_state')['presetAssigned']
            confirm_all()
            draft = work / 'sources.project.json'
            edit('save_project', dict(path=str(draft)))
            assert any('checklist' in v for v in records(draft).values()), 'Confirmations were not saved'
            assert not any('sources' in v for v in records(draft).values()), 'Source bytes were recorded'
            assert not any('step' in v or 'reviews' in v for v in records(draft).values()), 'Retired roadmap keys were saved'
            confirmed = states()

            # View-only changes keep every confirmation.
            edit('view', dict(view='side', fit=True))
            assert states() == confirmed, 'Camera changed the checklist'

            # An edited guide keeps every confirmation (owner, 2026-10-05: no
            # hash verification); the items show the new values.
            guide = command('magazine_guide_state')['settings']
            guide['travel'] *= 1.2
            edit('magazine_guide', dict(operation='save', settings=guide))
            ready()
            assert states() == confirmed, 'Editing the guide revoked confirmations'
            edit('magazine_guide', dict(operation='reset'))
            ready()
            assert states() == confirmed, 'Restoring the guide changed confirmations'

            # Hull edits never ask for the world model again; the overlay changes nothing.
            for empty in (False, True):
                edit('workspace', dict(index=5))
                ready()
                edit('capture_magazine_world', dict(empty=empty))
                collisions_ready()
            assert items()['feed.world']['canConfirm'], items()['feed.world']
            edit('checklist', dict(operation='confirm', id='feed.world'))
            world = states()
            edit('collisions', dict(operation='view', overlay=False))
            assert states() == world, 'Collision overlay changed the checklist'
            edit('collisions', dict(operation='hull', index=0, enabled=False))
            assert states()['feed.world'] != 'detected', states()['feed.world']
            edit('collisions', dict(operation='hull', index=0, enabled=True))
            assert states()['feed.world'] == 'confirmed', states()['feed.world']

            # Confirmations survive a save and reopen; unconfirm returns an item for review.
            edit('workspace', dict(index=5))
            ready()
            reviewed = work / 'confirmed.project.json'
            edit('save_project', dict(path=str(reviewed)))
            before = states()
            edit('open_project', dict(path=str(reviewed)))
            ready()
            assert states() == before, 'Reopening lost confirmations'
            edit('checklist', dict(operation='unconfirm', id='fire.motion'))
            assert states()['fire.motion'] == 'detected', states()['fire.motion']
            edit('checklist', dict(operation='confirm', id='fire.motion'))

            # Another sight is not a reload part: the same record and every
            # confirmation apply, also after a reopen.
            kept = states()
            sights = next(slot for slot in ready()['slots'] if slot['name'] == 'Sights')
            other = next(choice for choice in sights['choices'] if choice['id'] != sights['selected'])
            edit('configure', dict(slot=sights['key'], mod=other['id']))
            ready()
            edit('workspace', dict(index=5))
            ready()
            assert command('checklist_state')['evaluated'], command('checklist_state')['problem']
            assert states() == kept, 'Another sight changed the checklist'
            reopened = work / 'other-sight.project.json'
            edit('save_project', dict(path=str(reopened)))
            edit('open_project', dict(path=str(reopened)))
            ready()
            edit('workspace', dict(index=5))
            ready()
            assert states() == kept, 'Reopening with another sight changed the checklist'

            (work / 'result.json').write_text(json.dumps(dict(ok=True, states=before), indent=2))
            print('PASS checklist preset, kept confirmations through guide/collision edits, view independence, '
                  'reopen and parts outside the reload:', work, flush=True)
        finally:
            project.write_bytes(saved)
            command('open_project', dict(path=str(project)), command('revision')['revision'])
            ready()
            edit('save_project', dict(path=str(project)))
            project.write_bytes(saved)
    finally:
        if original:
            Path(original).write_bytes(original_bytes)
            command('open_project', dict(path=original), command('revision')['revision'])
            ready()
            edit('save_project', dict(path=original))
            Path(original).write_bytes(original_bytes)


if __name__ == '__main__':
    run()
