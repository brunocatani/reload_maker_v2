"""Rounds hidden when empty: the detected rounds, the author's choice, its
persistence, its effect on the pack and the checklist, and its reset.

Uses the Glock testbed, whose magazines carry one detected round (Bullet:0).
The testbed project and its checklist status are restored afterwards."""
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from template_mcp_test import ready, edit
from server import command


def runtime():
    return json.loads(Path(edit('export', dict(kind='runtime_configuration'))['path']).read_text(encoding='utf-8'))


def presentation_item():
    return next(i for i in command('checklist_state')['items'] if i['id'] == 'feed.presentation')


def refused(params, expected):
    try:
        edit('magazine_rounds', params)
    except RuntimeError as error:
        assert expected in str(error), error
    else:
        raise AssertionError('Accepted: %s' % params)


def run():
    original = ready()['project']
    original_bytes = Path(original).read_bytes()
    edit('testbed', dict(name='glock'))
    project = Path(ready()['project'])
    saved = project.read_bytes()
    status = project.with_name('status.json')
    status_bytes = status.read_bytes() if status.exists() else None
    try:
        edit('workspace', dict(index=5))
        ready()
        state = command('magazine_rounds_state')
        rounds = [p for p in state['pieces'] if p['round']]
        assert not state['setupDecides'] and not state['chosen'], state
        assert [p['name'] for p in rounds] == ['Bullet:0'] and rounds[0]['detected'], state['pieces']
        bullet = rounds[0]['path']
        # The pack hides the highest group holding only rounds.
        hidden = runtime()['magazine']['emptyHideNodes']
        assert hidden, 'The detected round is not hidden'
        confirmed = presentation_item()['state']
        assert 'rounds: 1 pieces' in presentation_item()['value'], presentation_item()['value']

        # The author makes the round a body: it stays visible when empty.
        result = edit('magazine_rounds', dict(operation='save', rounds={bullet: False}))
        ready()
        assert result['chosen'] == {bullet: False}, result
        assert not any(p['round'] for p in result['pieces'])
        exported = runtime()['magazine']
        assert 'emptyHideNodes' not in exported and exported['presentationSetup'] == {'rounds': {bullet: False}}, exported
        edit('save_project', dict(path=str(project)))  # nothing writes the project except Save (owner, 2026-10-05)
        persisted = json.loads(project.read_text(encoding='utf-8'))['magazineRounds']
        assert persisted == {state['key']: {bullet: False}}, persisted
        # A confirmation stays until the author removes it (owner, 2026-10-05).
        assert presentation_item()['state'] == confirmed, 'Changed rounds revoked the confirmation'

        # The choice survives reopening the project.
        edit('open_project', dict(path=str(project)))
        ready()
        edit('workspace', dict(index=5))
        ready()
        assert command('magazine_rounds_state')['chosen'] == {bullet: False}

        refused(dict(operation='save', rounds={'not/a/piece': True}), "among this magazine's pieces")
        everything = {p['path']: True for p in state['pieces']}
        refused(dict(operation='save', rounds=everything), 'at least one piece as the magazine body')
        refused(dict(operation='rename'), 'save or reset')

        # Back to the detected rounds: output and confirmation as before.
        result = edit('magazine_rounds', dict(operation='reset'))
        ready()
        assert not result['chosen'] and [p['name'] for p in result['pieces'] if p['round']] == ['Bullet:0']
        exported = runtime()['magazine']
        assert exported['emptyHideNodes'] == hidden and 'presentationSetup' not in exported, exported
        edit('save_project', dict(path=str(project)))
        assert 'magazineRounds' not in json.loads(project.read_text(encoding='utf-8'))
        assert presentation_item()['state'] == confirmed, presentation_item()
        print('PASS detected rounds, author choice, persistence, pack hiding, checklist review, refusals and reset', flush=True)
    finally:
        project.write_bytes(saved)
        if status_bytes is not None:
            status.write_bytes(status_bytes)
        command('open_project', dict(path=str(project)), command('revision')['revision'])
        ready()
        Path(original).write_bytes(original_bytes)
        command('open_project', dict(path=original), command('revision')['revision'])
        ready()


if __name__ == '__main__':
    run()
