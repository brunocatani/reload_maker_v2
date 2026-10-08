"""The editor harness never removes library work that existed before a run.

Runs restore() against a temporary library: a normal run puts changed files
back and removes only files the run added; a run that keeps its library keeps
everything. Pre-existing library files survive both, whatever the snapshot
listed (2026-10-08: a kept-library run snapshotted no library files and the
old cleanup deleted the whole library).
"""
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import editor_session as harness


def build(root):
    library = root/'library/plugins'
    files = {library/'a.esp/project.json': b'index', library/'a.esp/saves/1/snapshot.json': b'version',
             library/'b.esp/project.json': b'other'}
    for path, data in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return library, files


def run():
    saved = (harness.LIBRARY, harness.SESSIONS, harness.SETTINGS)
    try:
        for keep in (False, True):
            with tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                harness.LIBRARY, harness.SESSIONS, harness.SETTINGS = build(root)[0], root/'sessions', root/'settings.ini'
                _, files = build(root)
                existing = harness.library_files()
                # A kept-library run snapshots no library file at all.
                protected = harness.snapshot([] if keep else harness.corpus_files())
                (harness.LIBRARY/'a.esp/project.json').write_bytes(b'changed by the run')
                added = harness.LIBRARY/'a.esp/saves/2/snapshot.json'
                added.parent.mkdir(parents=True)
                added.write_bytes(b'new version')
                harness.restore(protected, existing, keep)
                for path in files:
                    assert path.exists(), (keep, 'pre-existing library file removed', path)
                assert added.exists() == keep, (keep, 'run file kept' if not keep else 'kept run file removed')
                expected = b'changed by the run' if keep else b'index'
                assert (harness.LIBRARY/'a.esp/project.json').read_bytes() == expected, keep
    finally:
        harness.LIBRARY, harness.SESSIONS, harness.SETTINGS = saved
    print('PASS editor harness restore: pre-existing library files always kept; run files removed only without keep_library', flush=True)


if __name__ == '__main__':
    run()
