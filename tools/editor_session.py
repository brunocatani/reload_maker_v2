"""Run the built editor for automated checks without changing the authoring corpus.

The editor saves its current session whenever it switches projects or closes, and
records the last opened project in its settings. Automated checks snapshot those
files first and restore any that changed once the editor has closed.
"""
import configparser
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'mcp'))
from server import command  # noqa: E402

EXECUTABLE = ROOT / 'build-fast/dist/ReloadMakerV2/ReloadMakerV2.exe'
PYTHON = ROOT / 'build-fast/dist/ReloadMakerV2/runtime/python/python.exe'
SETTINGS = ROOT / 'local/settings/ReloadMakerV2/ReloadMakerV2.ini'
ENDPOINT = ROOT / 'local/settings/endpoint.json'

TESTBEDS = (
    'glock', 'ru556', 'm1a', 'timberwolf', 'l118a2', 'thanatos',
    'srep-service', 'srep-ar10', 'srep-honeybadger', 'srep-xm16e1',
)

READY_TIMEOUT_SECONDS = 300
STARTUP_TIMEOUT_SECONDS = 120
CLOSE_TIMEOUT_SECONDS = 60


def ready():
    """Wait until the editor finishes loading, assembling and capturing."""
    deadline = time.monotonic() + READY_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        state = command('state')
        if not state['busy']:
            return state
        time.sleep(0.2)
    raise TimeoutError('Editor stayed busy for more than %d seconds' % READY_TIMEOUT_SECONDS)


def edit(method, params):
    return command(method, params, ready()['revision'])


def endpoint_alive():
    try:
        command('state')
        return True
    except Exception:
        return False


def launch_editor():
    if endpoint_alive():
        raise RuntimeError('Reload Maker is already running for this workspace; close it first')
    previous_endpoint = ENDPOINT.read_text(encoding='utf-8') if ENDPOINT.exists() else ''
    # Tests choose their own projects; restoring the last one could need MO2. Unattended keeps
    # the desktop's pointer, keyboard and focus out of the run, and the editor behind other windows.
    process = subprocess.Popen([str(EXECUTABLE), '--project', str(ROOT), '--no-restore', '--unattended'], cwd=str(ROOT))
    deadline = time.monotonic() + STARTUP_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        current = ENDPOINT.read_text(encoding='utf-8') if ENDPOINT.exists() else ''
        if current and current != previous_endpoint and endpoint_alive():
            ready()
            return process
        if process.poll() is not None:
            raise RuntimeError('Reload Maker exited during startup with code %d' % process.returncode)
        time.sleep(0.5)
    raise TimeoutError('Reload Maker did not open its command pipe')


def close_editor(process):
    # WM_CLOSE runs the editor's normal close path, including its session save.
    subprocess.run(['taskkill', '/PID', str(process.pid)], capture_output=True)
    try:
        process.wait(timeout=CLOSE_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        raise RuntimeError('Reload Maker did not close; a dialog may be waiting in its window')


def corpus_files():
    """The settings file plus every session the testbed switcher can open or save."""
    settings = configparser.ConfigParser(interpolation=None)
    settings.optionxform = str
    settings.read(SETTINGS, encoding='utf-8')
    files = [SETTINGS]
    keys = [('testbeds', name) for name in TESTBEDS] + [('projects', 'lastProject')]
    for section, key in keys:
        if settings.has_option(section, key):
            files.append(Path(settings.get(section, key)))
    return [path for path in files if path.exists()]


def snapshot(paths):
    return {path: path.read_bytes() for path in paths}


def restore(saved):
    restored = []
    for path, data in saved.items():
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
            restored.append(str(path))
    return restored


def files_written_since(folder, started):
    return sorted(str(path) for path in folder.rglob('*') if path.is_file() and path.stat().st_mtime > started)


class EditorRun:
    """Launch the editor for a block of checks and leave the corpus as it was.

    Session files are restored only after the editor has closed, because closing
    saves the current session.
    """

    def __enter__(self):
        self.protected = snapshot(corpus_files())
        self.started = time.time()
        self.process = launch_editor()
        self.restored = []
        return self

    def __exit__(self, *exception):
        try:
            close_editor(self.process)
        finally:
            self.restored = restore(self.protected)
        return False

    def relaunch_if_exited(self):
        """Start a new editor if the current one has exited; return its exit code, or None if it still runs."""
        code = self.process.poll()
        if code is None:
            return None
        self.process = launch_editor()
        return code

    def report(self):
        return {'restoredSessionFiles': self.restored,
                'otherWorkspaceFilesWritten': files_written_since(ROOT / 'workspace', self.started)}
