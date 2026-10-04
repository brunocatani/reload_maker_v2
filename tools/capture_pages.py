"""Capture every editor page as the user sees it, for comparing the UI with its
design. Runs an unattended editor on a testbed and restores the corpus after.

    python tools/capture_pages.py <out-folder> [--testbed glock] [--size 1600x980]
"""
import argparse
import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from editor_session import EditorRun, command, edit, ready

# (file name, workspace id, hand editor page or None)
PAGES = [
    ('operate', 5, None),
    ('builder-reload', 4, None),
    ('builder-part-grips', 2, 1),
    ('builder-mechanisms', 2, 2),
    ('builder-grab-points', 2, 3),
    ('builder-magazines', 7, None),
    ('poses-library', 2, 0),
    ('sounds', 6, None),
    ('scopes', 8, None),
    ('weapon-collision', 9, None),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('out', type=Path)
    parser.add_argument('--testbed', default='glock')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    captured = []
    with EditorRun() as run:
        edit('testbed', {'name': args.testbed})
        ready()
        for name, workspace, page in PAGES:
            params = {'index': workspace}
            if page is not None:
                params['handsPage'] = page
            edit('workspace', params)
            ready()
            result = command('window_capture')
            target = args.out / f'{name}.png'
            shutil.copyfile(result['path'], target)
            captured.append({'page': name, 'path': str(target), 'width': result['width'], 'height': result['height']})
            print(name, target, flush=True)
    (args.out / 'pages.json').write_text(json.dumps({'pages': captured, 'editor': run.report()}, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
