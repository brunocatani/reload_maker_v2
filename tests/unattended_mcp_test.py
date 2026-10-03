"""Desktop input does not reach the document of an editor launched with --unattended.

The suite runner launches the editor unattended. While a magazine is carried, mouse
wheel input over the viewport normally moves it onto the rail and changes the document
revision outside any command, which is how someone using the same desktop used to
break running suites. The wheel messages are posted to the editor's own windows; the
real pointer and the real focus are not touched.
"""
import ctypes
import json
import sys
import time
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'mcp'))
from server import command  # noqa: E402

WM_MOUSEWHEEL = 0x020A
WHEEL_NOTCH = 120
WHEEL_NOTCHES = 3
SETTLE_SECONDS = 0.5

user32 = ctypes.windll.user32
ENUMERATE = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


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


def simulate(operation):
    return command('simulate', {'operation': operation}, ready()['revision'])


def viewport_windows(process_id):
    """The native child windows of the editor's visible top-level windows; the 3D viewport is one."""
    found = []

    def each_child(window, _):
        found.append(window)
        return True

    def each_top_level(window, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == process_id and user32.IsWindowVisible(window):
            user32.EnumChildWindows(window, ENUMERATE(each_child), 0)
        return True

    user32.EnumWindows(ENUMERATE(each_top_level), 0)
    return found


def post_wheel(window):
    area = wintypes.RECT()
    user32.GetWindowRect(window, ctypes.byref(area))
    # Wheel messages carry screen coordinates: the middle of the window.
    x = (area.left + area.right) // 2
    y = (area.top + area.bottom) // 2
    position = (x & 0xFFFF) | ((y & 0xFFFF) << 16)
    for _ in range(WHEEL_NOTCHES):
        user32.PostMessageW(window, WM_MOUSEWHEEL, WHEEL_NOTCH << 16, position)


def document():
    state = command('state')
    magazine = command('simulation_state')['magazine']
    return {'revision': state['revision'], 'phase': magazine['phase'], 'distance': magazine['distance']}


def main():
    endpoint = json.loads((ROOT / 'local/settings/endpoint.json').read_text(encoding='utf-8'))
    for operation in ('reset', 'drop_magazine', 'grab_magazine'):
        held = simulate(operation)
    assert held['magazineGrip']['applied'], 'The magazine is not carried'
    windows = viewport_windows(endpoint['pid'])
    assert windows, 'The editor has no viewport window'
    before = document()
    for window in windows:
        post_wheel(window)
    time.sleep(SETTLE_SECONDS)
    after = document()
    assert after == before, ('Desktop wheel input changed the document', before, after)
    simulate('reset')
    print('PASS desktop wheel input over the viewport leaves a carried magazine and the revision unchanged')


if __name__ == '__main__':
    main()
