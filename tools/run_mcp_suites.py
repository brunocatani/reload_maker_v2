"""Run the Python integration suites against one launched editor.

Offline suites run first without an editor. Editor suites then run in sequence
against a single editor instance; session files the editor rewrites are restored
when it closes. Suites that need external inputs are reported as skipped until
their inputs are configured here.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

# The bundled runtime is isolated and does not add the script folder to the path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from editor_session import PYTHON, ROOT, EditorRun, edit, ready

TESTS = ROOT / 'tests'
SUITE_TIMEOUT_SECONDS = 1800

OFFLINE = ('coverage_test.py', 'source_requirements_test.py')

# Suites with required command-line inputs that are not yet configured.
NEEDS_INPUTS = {
    'asset_cook_test.py': '--model and --hulls',
    'home_mcp_test.py': '--pack',
    'installation_mcp_test.py': 'installation paths',
    'multiweapon_pack_test.py': '--capture and --assets',
    'pack_builder_test.py': '--application, --workspace and --pack',
    'pack_job_mcp_test.py': '--pack',
    'portable_tools_test.py': '--application, --workspace and --nif',
    'product_authoring_test.py': '--captures',
    'shared_magazine_mcp_test.py': '--project',
}


def editor_suites():
    names = sorted(path.name for path in TESTS.glob('*_test.py'))
    return [name for name in names if name not in OFFLINE and name not in NEEDS_INPUTS]


def open_reference_project():
    """Editor suites were written against an open Glock testbed; each starts from it."""
    edit('testbed', {'name': 'glock'})
    state = ready()
    if state['lastError']:
        raise RuntimeError(state['lastError'])


def run_editor_suite(name, logs):
    try:
        open_reference_project()
    except Exception as error:
        result = {'suite': name, 'status': 'blocked', 'seconds': 0, 'last': 'Could not open the Glock testbed: %s' % error}
        print('%-8s %-38s %s' % ('blocked', name, result['last'][:120]), flush=True)
        return result
    return run_suite(name, logs)


def run_editor_suites(names, logs):
    """Run suites against one editor; if it exits, blame the running suite and start a new editor."""
    results = []
    with EditorRun() as run:
        for name in names:
            result = run_editor_suite(name, logs)
            exit_code = run.relaunch_if_exited()
            if exit_code is not None:
                result['status'] = 'editor-exited'
                result['last'] = 'Editor exited with code %d during this suite; %s' % (exit_code, result['last'])
                print('%-8s %-38s %s' % ('exited', name, result['last'][:120]), flush=True)
            results.append(result)
    return results, run.report()


def run_suite(name, logs):
    started = time.monotonic()
    log = logs / (name[:-3] + '.log')
    try:
        completed = subprocess.run([str(PYTHON), '-B', str(TESTS / name)], cwd=str(ROOT), capture_output=True,
                                   text=True, encoding='utf-8', errors='replace', timeout=SUITE_TIMEOUT_SECONDS)
        output = completed.stdout + completed.stderr
        status = 'passed' if completed.returncode == 0 else 'failed'
    except subprocess.TimeoutExpired as timeout:
        # Captured output on a timeout is bytes even with text=True.
        output = b''.join(part or b'' for part in (timeout.stdout, timeout.stderr)).decode('utf-8', 'replace')
        status = 'timeout'
    log.write_text(output, encoding='utf-8')
    last_line = next((line for line in reversed(output.splitlines()) if line.strip()), '')
    result = {'suite': name, 'status': status, 'seconds': round(time.monotonic() - started, 1), 'last': last_line[:300]}
    print('%-8s %-38s %6.1fs  %s' % (status, name, result['seconds'], result['last'][:120]), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--suite', action='append', help='Run only these suite files')
    parser.add_argument('--out', type=Path, default=ROOT / 'local/test-results' / time.strftime('%Y%m%d-%H%M%S'))
    parser.add_argument('--baseline', type=Path, help='Earlier results folder; report suites whose status changed')
    args = parser.parse_args()
    if args.baseline:
        baseline = statuses(args.baseline)
    logs = args.out.resolve()
    logs.mkdir(parents=True, exist_ok=True)
    selected = set(args.suite or [])
    chosen = lambda names: [name for name in names if not selected or name in selected]  # noqa: E731

    results = [run_suite(name, logs) for name in chosen(OFFLINE)]
    editor = chosen(editor_suites())
    report = {}
    if editor:
        editor_results, report = run_editor_suites(editor, logs)
        results += editor_results
    skipped = [{'suite': name, 'status': 'skipped', 'needs': needs}
               for name, needs in NEEDS_INPUTS.items() if not selected or name in selected]
    summary = {'results': results + skipped, **report}
    (logs / 'summary.json').write_text(json.dumps(summary, indent=1) + '\n', encoding='utf-8')
    counts = {}
    for entry in summary['results']:
        counts[entry['status']] = counts.get(entry['status'], 0) + 1
    print(json.dumps(counts), 'logs:', logs)
    if args.baseline:
        # A refactor must not change any suite's outcome, including known failures.
        changed = {name: (baseline.get(name), status) for name, status in statuses(logs).items()
                   if baseline.get(name) != status}
        for name, (before, after) in sorted(changed.items()):
            print('CHANGED %-38s %s -> %s' % (name, before, after))
        print('%d suites changed status against %s' % (len(changed), args.baseline))
        return 1 if changed else 0
    return 0 if all(entry['status'] in ('passed', 'skipped') for entry in summary['results']) else 1


def statuses(folder):
    summary = json.loads((Path(folder) / 'summary.json').read_text(encoding='utf-8'))
    return {entry['suite']: entry['status'] for entry in summary['results']}


if __name__ == '__main__':
    sys.exit(main())
