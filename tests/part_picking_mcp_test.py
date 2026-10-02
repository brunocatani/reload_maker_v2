"""Clicking the rendered weapon resolves to the loadout slot that installed the part.

Runs against the editor through the commands the viewport uses: part_at reports the
installed part under a viewport pixel, focus_slot focuses a loadout slot.
"""
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'mcp'))
from server import command  # noqa: E402

GRID_STEP_PIXELS = 32


def ready():
    deadline = time.monotonic() + 300
    while time.monotonic() < deadline:
        state = command('state')
        if not state['busy']:
            if state['lastError']:
                raise RuntimeError(state['lastError'])
            return state
        time.sleep(0.1)
    raise RuntimeError('Editor stayed busy')


def edit(method, params):
    return command(method, params, ready()['revision'])


def rejects(method, params):
    try:
        command(method, params, ready()['revision'])
    except RuntimeError:
        return True
    return False


def main():
    edit('testbed', {'name': 'glock'})
    state = ready()
    slots = {slot['key']: slot for slot in state['slots']}

    edit('view', {'view': 'side', 'fit': True})
    corner = command('part_at', {'x': 0, 'y': 0})
    assert corner['slot'] is None, ('The empty corner must not pick a part', corner)
    width, height = corner['width'], corner['height']
    for invalid in ({'x': -1, 'y': 5}, {'x': 5}, {'x': width, 'y': 5}, {'x': 5, 'y': height}):
        assert rejects('part_at', invalid), 'part_at accepted an invalid point: %s' % invalid

    found = {}
    for y in range(GRID_STEP_PIXELS // 2, height, GRID_STEP_PIXELS):
        for x in range(GRID_STEP_PIXELS // 2, width, GRID_STEP_PIXELS):
            hit = command('part_at', {'x': x, 'y': y})
            if hit['slot'] is None:
                continue
            assert hit['slot'] in slots, ('Picked slot is not in the configuration', hit)
            assert hit['label'] == slots[hit['slot']]['name'], hit
            assert hit['part'], ('An installed part has a name', hit)
            assert hit['slot'] != '0', hit
            found.setdefault(hit['slot'], (x, y, hit))
    names = sorted(slots[key]['name'] for key in found)
    assert len(found) >= 2, ('Expected several distinct installed parts in the side view', names)

    # Picking is a pure query: it must not change the document.
    assert ready()['revision'] == state['revision'] + 1, 'part_at changed the document revision'

    target = sorted(found)[0]
    focused = edit('focus_slot', {'slot': target})
    assert focused['focusedSlot'] == target, focused
    assert rejects('focus_slot', {'slot': '123'}), 'focus_slot accepted a slot outside the configuration'
    assert rejects('focus_slot', {'slot': int(target)}), 'focus_slot accepted a numeric slot key'

    x, y, hit = found[target]
    print('PASS part picking: %d slots reachable (%s); %s at (%d, %d); focus round trip'
          % (len(found), ', '.join(names), hit['label'], x, y))


if __name__ == '__main__':
    main()
