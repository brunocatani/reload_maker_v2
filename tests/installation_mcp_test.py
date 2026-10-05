"""Exercise saved setup, preserved projects and optional real MO2 reconnect."""
import argparse
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'mcp'))
from server import command


def idle(timeout=120):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        try:
            state = command('state')
            if not state['busy']:
                assert not state['lastError'], state['lastError']
                return state
        except (OSError, RuntimeError):
            pass  # The process/pipe changes exactly once during handoff.
        time.sleep(.25)
    raise AssertionError('Editor did not finish loading')


def run():
    parser = argparse.ArgumentParser()
    for name in ('game', 'kit', 'mo2', 'profile'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--reconnect', action='store_true')
    args = parser.parse_args()
    original = idle()
    setup = dict(version=1, gameRoot=args.game, creationKitRoot=args.kit,
                 sameToolsRoot=False, mo2Instance=args.mo2, profile=args.profile)
    before = command('installation')['settings']
    try:
        command('configure_installation', dict(setup, profile='../bad'), original['revision'])
        raise AssertionError('Traversal profile accepted')
    except RuntimeError as exc:
        assert 'profile folder' in str(exc), str(exc)
    assert command('installation')['settings'] == before
    result = command('configure_installation', setup, command('revision')['revision'])
    assert result['gameReady'] and result['collisionReady'] and result['profileReady'], result
    assert command('state')['project'] == original['project']
    output = dict(setup=result)
    if args.reconnect:
        command('connect_profile', revision=command('revision')['revision'])
        until = time.monotonic() + 40
        while time.monotonic() < until:
            try:
                # A client with the old session must fail before sending an edit.
                command('installation')
            except RuntimeError as exc:
                assert 'session changed' in str(exc).lower(), str(exc)
                break
            except OSError:
                pass
            time.sleep(.2)
        else:
            raise AssertionError('Reconnect did not replace the editor session')
        loaded = idle()
        assert loaded['session'] != original['session'] and loaded['library'], loaded
        connected = command('installation')
        assert connected['virtualFilesystem'] and connected['settings']['profile'] == args.profile
        endpoint = json.loads((ROOT/'local/settings/endpoint.json').read_text())
        with open('\\\\.\\pipe\\'+endpoint['pipe'], 'r+b', buffering=0) as pipe:
            pipe.write(json.dumps(dict(method='focus', expectedSession=original['session'])).encode()+b'\n')
            data = bytearray()
            while not data.endswith(b'\n'): data.extend(pipe.read(65536))
        assert not json.loads(data)['ok'], 'Old session accepted by new editor'
        output['connected'] = {k: loaded[k] for k in ('session', 'library', 'busy', 'lastError')}
        output['weaponCount'] = len(loaded['weapons'])
    destination = ROOT/'evidence/setup'
    destination.mkdir(parents=True, exist_ok=True)
    (destination/'integration.json').write_text(json.dumps(output, indent=2))
    print('PASS setup round trip, invalid input, preserved project' +
          (', verified MO2 handoff and stale session rejection' if args.reconnect else ''))


if __name__ == '__main__':
    run()
