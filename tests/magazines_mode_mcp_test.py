"""Reload builder › Magazines works the magazine in place, without Operate.

Workspace 10 shows the Pieces and Insertion guide sub-steps on the prepared
magazine. A typed guide edit is applied to the prepared rail live and gives
exactly the rail a full preparation from the saved guide gives; an edit that
changes nothing marks nothing. A Pieces role on the full magazine is the
magazine's round choice; an animation-only piece makes the configuration's
setup. Hand fit and World models report the same step from their pages.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
from template_mcp_test import ready, edit, simulation
from server import command

TOLERANCE = 1e-4


def close(a, b, path='rail'):
    """Equal JSON, numbers within TOLERANCE."""
    if isinstance(a, dict):
        assert isinstance(b, dict) and a.keys() == b.keys(), (path, sorted(a), sorted(b) if isinstance(b, dict) else b)
        for key in a:
            close(a[key], b[key], f'{path}.{key}')
    elif isinstance(a, list):
        assert isinstance(b, list) and len(a) == len(b), (path, a, b)
        for i, (x, y) in enumerate(zip(a, b)):
            close(x, y, f'{path}[{i}]')
    elif isinstance(a, (int, float)) and not isinstance(a, bool):
        assert abs(a - b) <= TOLERANCE * max(1, abs(a)), (path, a, b)
    else:
        assert a == b, (path, a, b)


def open_step(step):
    edit('workspace', dict(index=10, magazineStep=step))
    state = ready()
    assert state['workspace'] == 10 and state['magazineStep'] == step, (state['workspace'], state['magazineStep'])
    return state


def run(name):
    edit('testbed', dict(name=name)); ready()
    state = open_step(1)
    assert state['modified'] is False, 'opening Magazines marked the weapon'
    settings = command('magazine_guide_state')['settings']

    # The same guide typed again changes nothing.
    edit('magazine_guide', dict(operation='edit', settings=settings))
    assert ready()['modified'] is False, 'an unchanged guide marked the weapon'

    # A typed guide applies live, on this page.
    revised = dict(settings, outwardInConnection=[.6, 0, -.8], travel=settings['travel']*1.25)
    edit('magazine_guide', dict(operation='edit', settings=revised))
    state = ready()
    assert state['workspace'] == 10 and state['magazineStep'] == 1, 'a guide edit left Magazines'
    assert state['modified'] is True, 'a guide edit did not mark the weapon'
    live = simulation()['magazineInteraction']
    assert abs(live['travel'] - revised['travel']) < TOLERANCE, live['travel']

    # Preparing the magazine again from the saved guide gives the same rail.
    edit('workspace', dict(index=4)); ready()
    open_step(1)
    close(live, simulation()['magazineInteraction'])

    # Pieces: a round mark on the full magazine is the magazine's choice; an
    # animation-only piece is this configuration's setup.
    open_step(0)
    pieces = command('magazine_setup_state')['pieces']
    round_piece = next(p for p in pieces if p['role'] == 'cartridge' and not p['empty'])
    edit('magazine_setup', dict(operation='role', path=round_piece['path'], role='body')); ready()
    rounds = command('magazine_rounds_state')
    assert not rounds['setupDecides'] and False in rounds['chosen'].values(), rounds['chosen']
    edit('magazine_setup', dict(operation='role', path=round_piece['path'], role='cartridge')); ready()
    assert not command('magazine_rounds_state')['chosen'], 'restoring the role kept a round choice'
    if True:
        edit('magazine_setup', dict(operation='role', path=round_piece['path'], role='spare')); ready()
        setup = command('magazine_setup_state')
        assert setup['saved'], 'an animation-only piece did not make the setup'
        assert next(p for p in setup['pieces'] if p['path'] == round_piece['path'])['role'] == 'spare'
        edit('magazine_setup', dict(operation='reset')); ready()

    # The other sub-steps report Magazines from their own pages.
    edit('workspace', dict(index=7)); assert ready()['magazineStep'] == 3
    edit('workspace', dict(index=5)); assert ready()['magazineStep'] == -1
    # Leaving the weapon discards the unsaved edits.
    edit('testbed', dict(name='glock')); ready()
    print('PASS', name, flush=True)


if __name__ == '__main__':
    for testbed in ('ru556', 'glock'):
        run(testbed)
    print('PASS magazines mode: sub-steps in place, live typed guide equals a fresh preparation, '
          'unchanged edits mark nothing, piece roles routed to rounds or setup', flush=True)
