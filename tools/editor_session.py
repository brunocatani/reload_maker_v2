"""Run the built editor for automated checks without changing the authoring corpus.

Nothing writes the ESP library except a deliberate save, and an unattended
editor discards unsaved edits when it changes source or closes. Automated
checks still record the settings file and the library first, then restore the
settings and remove every library file a check added once the editor has
closed. Library records and assets are content-named and never rewritten, so
only the small index files need their bytes kept.
"""
import configparser
import os
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

# The developer testbeds by id (workspace/library/development-sources.json).
TESTBEDS = (
    'glock', 'ru556', 'm1a', 'timberwolf', 'l118a2', 'thanatos',
    'srep-service', 'srep-ar10', 'srep-honeybadger', 'srep-xm16e1',
)
LIBRARY = ROOT / 'workspace/library'
# Where each weapon was left (loadout, camera, page), written by the editor.
SESSIONS = ROOT / 'local/sessions'

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
    arguments = [str(EXECUTABLE), '--project', str(ROOT), '--no-restore', '--unattended']
    # Parallel suite runners share one memory budget for sources kept loaded.
    if os.environ.get('RELOADMAKER_RESIDENT_MB'):
        arguments += ['--resident-memory-mb', os.environ['RELOADMAKER_RESIDENT_MB']]
    process = subprocess.Popen(arguments, cwd=str(ROOT))
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


def session_files():
    return sorted(path for path in SESSIONS.glob('*.json') if path.is_file()) if SESSIONS.exists() else []


def corpus_files():
    """The settings file, the weapon sessions and every ESP library file. Only
    the files that are replaced in place (settings, sessions, indexes, export
    identity and history, the development sources) have their bytes kept; the
    rest only need to be known."""
    files = ([SETTINGS] if SETTINGS.exists() else []) + session_files()
    return files + sorted(path for path in LIBRARY.rglob('*') if path.is_file()) if LIBRARY.exists() else files


# The sessions as they were before any editor of this run; reset_sessions
# returns to them so one suite's loadouts never reach the next.
SESSION_BASELINE = {path: path.read_bytes() for path in session_files()}


def reset_sessions():
    for path in session_files():
        if path not in SESSION_BASELINE:
            path.unlink()
    for path, data in SESSION_BASELINE.items():
        if not path.exists() or path.read_bytes() != data:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)


def replaced_in_place(path):
    relative = path.relative_to(LIBRARY).parts if path.is_relative_to(LIBRARY) else ()
    return not relative or not any(part in ('saves', 'records', 'assets') for part in relative[:-1])


def snapshot(paths):
    return {path: path.read_bytes() if replaced_in_place(path) else True for path in paths}


def library_files():
    return {path for path in LIBRARY.rglob('*') if path.is_file()} if LIBRARY.exists() else set()


def restore(saved, existing, keep_library=False):
    """Puts the protected files back and removes what the run added.

    existing is every library file present when the run started (always
    recorded, also when the library is kept). Only files absent from it can
    ever be deleted, so a run can never remove library work that was there
    before it; keep_library keeps the run's own saves as well."""
    restored = []
    for path, data in saved.items():
        if data is True:
            continue
        if not path.exists() or path.read_bytes() != data:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            restored.append(str(path))
    for path in session_files():
        if path not in saved:
            path.unlink()
            restored.append(str(path))
    if LIBRARY.exists() and not keep_library:
        for path in sorted(LIBRARY.rglob('*'), reverse=True):
            if path.is_file() and path not in existing:
                path.unlink()
                restored.append(str(path))
            elif path.is_dir() and not any(path.iterdir()):
                path.rmdir()
    return restored


def files_written_since(folder, started):
    return sorted(str(path) for path in folder.rglob('*') if path.is_file() and path.stat().st_mtime > started)


class EditorRun:
    """Launch the editor for a block of checks and leave the corpus as it was.

    Session files are restored only after the editor has closed, when every
    explicit save of the run has landed. keep_library=True is for authoring
    runs whose saves are meant to stay: only the settings and the weapon
    sessions are restored.
    """

    def __init__(self, keep_library=False):
        self.keep_library = keep_library

    def __enter__(self):
        kept = ([SETTINGS] if SETTINGS.exists() else []) + session_files()
        self.protected = snapshot(kept if self.keep_library else corpus_files())
        self.existing = library_files()
        self.started = time.time()
        # Set before launching: report() must still describe a failed launch.
        self.restored = []
        self.process = launch_editor()
        return self

    def __exit__(self, *exception):
        try:
            close_editor(self.process)
        finally:
            self.restored = restore(self.protected, self.existing, self.keep_library)
        return False

    def relaunch_if_exited(self):
        """Start a new editor if the current one has exited; return its exit code, or None if it still runs."""
        code = self.process.poll()
        if code is None:
            return None
        self.process = launch_editor()
        return code

    def report(self):
        return {'restoredCorpusFiles': self.restored,
                'otherWorkspaceFilesWritten': files_written_since(ROOT / 'workspace', self.started)}
