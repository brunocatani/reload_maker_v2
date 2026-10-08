"""The checklist shows what exists and what is missing, with no confirmations.

Preset assignment, items done from the weapon's own data outside Operate,
guide and collision edits, camera and overlay independence, optional skips,
reopen, and parts outside the reload.
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


def missing_required():
    return sorted(key for key, item in items().items() if item['state'] == 'missing' and item['mandatory'])


def records(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))['authoringRoadmaps']


def run():
    work = ROOT / 'local/checklist-tests' / uuid.uuid4().hex
    work.mkdir(parents=True)
    original = command('export',{'kind':'project'},ready()['revision'])['path']
    original_bytes = Path(original).read_bytes() if original else None
    try:
        edit('testbed', dict(name='glock'))
        project = Path(command('export',{'kind':'project'},ready()['revision'])['path'])
        saved = project.read_bytes()
        try:
            edit('workspace', dict(index=5))
            ready()
            state = command('checklist_state')
            assert state['evaluated'], state['problem']
            assert {preset['id'] for preset in state['presets']} >= {state['preset']}, state['presets']
            edit('checklist', dict(operation='preset', id=state['preset']))
            ready()
            state = command('checklist_state')
            assert state['presetAssigned'] and state['items'][0]['state'] == 'done', state['items'][0]
            # No item waits for a confirmation; nothing confirms.
            assert not {'detected', 'confirmed', 'needs-you'} & set(states().values()), states()
            assert 'toConfirm' not in state['summary'] and state['summary']['missing'] == len(missing_required()), state['summary']
            try:
                edit('checklist', dict(operation='confirm', id='fire.motion'))
            except RuntimeError:
                pass
            else:
                raise AssertionError('The retired confirm operation was accepted')
            done = states()
            draft = work / 'sources.project.json'
            edit('export', dict(kind='project', path=str(draft)))
            assert not any('sources' in v for v in records(draft).values()), 'Source bytes were recorded'
            assert not any('step' in v or 'reviews' in v for v in records(draft).values()), 'Retired roadmap keys were saved'

            # Outside Operate the same data is done, not "checked in Operate".
            edit('workspace', dict(index=2))
            ready()
            outside = states()
            assert 'not-checked' not in outside.values(), outside
            assert outside['feed.guide'] == done['feed.guide'] == 'done', (outside['feed.guide'], done['feed.guide'])
            edit('workspace', dict(index=5))
            ready()

            # View-only changes keep every state.
            edit('view', dict(view='side', fit=True))
            assert states() == done, 'Camera changed the checklist'

            # An edited guide stays done; the item shows the new value.
            guide = command('magazine_guide_state')['settings']
            guide['travel'] *= 1.2
            edit('magazine_guide', dict(operation='save', settings=guide))
            ready()
            assert states() == done, 'Editing the guide changed the checklist'
            edit('magazine_guide', dict(operation='reset'))
            ready()
            assert states() == done, 'Restoring the guide changed the checklist'

            # Hulls: missing without them, done with them; the overlay changes nothing.
            for empty in (False, True):
                edit('workspace', dict(index=5))
                ready()
                edit('capture_magazine_world', dict(empty=empty))
                collisions_ready()
            assert states()['feed.world'] == 'done', items()['feed.world']
            world = states()
            edit('collisions', dict(operation='view', overlay=False))
            assert states() == world, 'Collision overlay changed the checklist'
            # Hull edits never ask for the world model again.
            edit('collisions', dict(operation='hull', index=0, enabled=False))
            assert states()['feed.world'] == 'done', states()['feed.world']
            edit('collisions', dict(operation='hull', index=0, enabled=True))
            assert states()['feed.world'] == 'done', states()['feed.world']
            # A missing optional item may be marked not applicable, and back;
            # a required one may not.
            optional = next((key for key, item in items().items() if item['state'] == 'missing' and not item['mandatory']), None)
            if optional:
                edit('checklist', dict(operation='skip', id=optional))
                assert states()[optional] == 'not-applicable', states()[optional]
                edit('checklist', dict(operation='unskip', id=optional))
                assert states()[optional] == 'missing', states()[optional]
            try:
                edit('checklist', dict(operation='skip', id='feed.guide'))
            except RuntimeError:
                pass
            else:
                raise AssertionError('A required item was skipped')

            # The states survive an export and import of the weapon's authoring.
            edit('workspace', dict(index=5))
            ready()
            reviewed = work / 'reviewed.project.json'
            edit('export', dict(kind='project', path=str(reviewed)))
            before = states()
            edit('import_project', dict(path=str(reviewed)))
            ready()
            assert states() == before, 'Reopening changed the checklist'

            # Another sight is not a reload part: the same record and every
            # state apply, also after a reopen.
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
            edit('export', dict(kind='project', path=str(reopened)))
            edit('import_project', dict(path=str(reopened)))
            ready()
            edit('workspace', dict(index=5))
            ready()
            assert states() == kept, 'Reopening with another sight changed the checklist'

            (work / 'result.json').write_text(json.dumps(dict(ok=True, states=before), indent=2))
            print('PASS checklist without confirmations: preset, done outside Operate, guide/collision edits, '
                  'optional skip, view independence, reopen and parts outside the reload:', work, flush=True)
        finally:
            project.write_bytes(saved)
            command('import_project', dict(path=str(project)), command('revision')['revision'])
            ready()
            edit('export', dict(kind='project',path=str(project)))
            project.write_bytes(saved)
    finally:
        if original:
            Path(original).write_bytes(original_bytes)
            command('import_project', dict(path=original), command('revision')['revision'])
            ready()
            edit('export', dict(kind='project',path=original))
            Path(original).write_bytes(original_bytes)


if __name__ == '__main__':
    run()
