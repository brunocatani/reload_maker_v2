"""Run the Python integration suites against launched editors.

Offline suites run first without an editor. Editor suites then run on several
editors at once: each editor works in a mirror of this workspace with its own
copy of the authoring corpus and settings, so this workspace's own files are
never written. With --jobs 1 the suites run in sequence against one editor in
this workspace, and session files the editor rewrites are restored when it
closes. Suites that need external inputs are reported as skipped until their
inputs are configured here.

Some suites pass or fail depending on which suites ran before them in the same
editor. A comparison with --baseline therefore runs the suites in the baseline's
own worker split and order; --rebalance splits them afresh from the baseline's
timings, for recording a new baseline.
"""
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

# The bundled runtime is isolated and does not add the script folder to the path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from editor_session import PYTHON, ROOT, EditorRun, command, edit, ready

TESTS = ROOT / 'tests'
SUITE_TIMEOUT_SECONDS = 1800

OFFLINE = ('esp_library_test.py', 'presentation_identity_test.py', 'catalog_slots_test.py')

# Suites with required command-line inputs that are not yet configured.
NEEDS_INPUTS = {
    'asset_cook_test.py': '--model and --hulls',
    'home_mcp_test.py': '--plugin',
    'installation_mcp_test.py': 'installation paths',
    'multiweapon_pack_test.py': '--capture and --assets',
    'pack_builder_test.py': '--application, --workspace and --pack',
    'pack_job_mcp_test.py': '--plugin',
    'portable_tools_test.py': '--application, --workspace and --nif',
    'product_authoring_test.py': '--captures',
    'shared_magazine_mcp_test.py': '--project',
    # These read the MO2 profile's plugin list and load order. The runner's
    # editors run outside MO2, where Maker never reads MO2 (owner rule); run
    # them by hand from Maker launched inside the selected MO2 profile.
    'player_library_mcp_test.py': 'Maker running inside MO2',
    'source_browser_mcp_test.py': 'Maker running inside MO2',
}

# The workspace rule for tests: at most four processes' worth of work at once.
MAXIMUM_JOBS = 4
WORKERS = ROOT / 'local/test-workers'
MIRRORED_CODE = ('tests', 'mcp', 'tools')
# These suites run the Creation Kit cook tool. They stay on one worker because
# two cooks at the same time are untested.
COOK_SUITES = ('live_pack_mcp_test.py', 'magazine_piece_mcp_test.py', 'magazine_world_mcp_test.py',
               'visible_geometry_mcp_test.py')
STALE_EDITOR_TIMEOUT_SECONDS = 60
RESIDENT_MEMORY_MB = 16 * 1024


def editor_suites():
    names = sorted(path.name for path in TESTS.glob('*_test.py'))
    return [name for name in names if name not in OFFLINE and name not in NEEDS_INPUTS]


def open_reference_project():
    """Editor suites were written against an open Glock testbed; each starts from
    its latest saved version. Selecting the testbed already open keeps its
    working state, so loading the latest version discards what the previous
    suite left and rebuilds the weapon."""
    edit('testbed', {'name': 'glock'})
    state = ready()
    if state['lastError']:
        raise RuntimeError(state['lastError'])
    latest = command('esp_project')['latest']
    if latest:
        edit('esp_version', {'operation': 'load', 'save': latest})
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
    run = EditorRun()
    try:
        with run:
            for name in names:
                result = run_editor_suite(name, logs)
                exit_code = run.relaunch_if_exited()
                if exit_code is not None:
                    result['status'] = 'editor-exited'
                    result['last'] = 'Editor exited with code %d during this suite; %s' % (exit_code, result['last'])
                    print('%-8s %-38s %s' % ('exited', name, result['last'][:120]), flush=True)
                results.append(result)
    except RuntimeError as error:
        # The suites have run; an editor that will not close must not discard their results.
        print('EDITOR   %s' % error, flush=True)
        return results, {**run.report(), 'editorError': str(error)}
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


def close_stale_editor(worker):
    """An editor left in a worker by an interrupted run would refuse a second launch; end it."""
    try:
        endpoint = json.loads((worker / 'local/settings/endpoint.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return
    pipe = '\\\\.\\pipe\\' + str(endpoint.get('pipe', ''))

    def alive():
        try:
            with open(pipe, 'r+b', buffering=0):
                return True
        except OSError:
            return False

    if not alive():
        return
    # A worker's corpus is a copy that the next step replaces, so the editor's closing save is not needed.
    subprocess.run(['taskkill', '/F', '/PID', str(endpoint.get('pid', 0))], capture_output=True)
    deadline = time.monotonic() + STALE_EDITOR_TIMEOUT_SECONDS
    while alive():
        if time.monotonic() > deadline:
            raise RuntimeError('An editor from an earlier test run still owns %s; close it first' % worker)
        time.sleep(0.5)


def prepare_worker(index):
    """Mirror this workspace for one more editor.

    The worker gets its own copy of the authoring corpus and settings, and its own
    scratch, export and evidence folders, so suites running at the same time never
    share a document or a session file. The build is shared through a junction.
    Nothing here is removed recursively through that junction: only the copied
    code and settings folders are replaced.
    """
    worker = WORKERS / ('w%d' % index)
    close_stale_editor(worker)
    worker.mkdir(parents=True, exist_ok=True)
    for name in MIRRORED_CODE:
        shutil.rmtree(worker / name, ignore_errors=True)
        shutil.copytree(ROOT / name, worker / name, ignore=shutil.ignore_patterns('__pycache__'))
    build = worker / 'build-fast'
    if not build.exists():
        subprocess.run(['cmd', '/c', 'mklink', '/J', str(build), str(ROOT / 'build-fast')], check=True, capture_output=True)
    mirror = subprocess.run(['robocopy', str(ROOT / 'workspace'), str(worker / 'workspace'), '/MIR', '/R:1', '/W:1',
                             '/XJ', '/NFL', '/NDL', '/NJH', '/NJS', '/NP'], capture_output=True)
    # Robocopy reports copied or extra files with codes below 8; 8 and above are failures.
    if mirror.returncode >= 8:
        raise RuntimeError('Could not mirror the workspace into %s' % worker)
    settings = worker / 'local/settings'
    shutil.rmtree(settings, ignore_errors=True)
    shutil.copytree(ROOT / 'local/settings', settings, ignore=shutil.ignore_patterns('endpoint.json'))
    # Settings that name this workspace's files must name the worker's copies.
    ini = settings / 'ReloadMakerV2/ReloadMakerV2.ini'
    own_root = re.compile(re.escape(ROOT.as_posix() + '/'), re.IGNORECASE)
    ini.write_text(own_root.sub(lambda match: worker.as_posix() + '/', ini.read_text(encoding='utf-8')), encoding='utf-8')
    (worker / 'local/tmp').mkdir(parents=True, exist_ok=True)
    return worker


def partition(names, jobs, seconds):
    """Split the suites between workers: longest first, each to the worker with the least work so far.

    Suites without a known duration count as the median. The cook suites move together.
    """
    typical = statistics.median(seconds.values()) if seconds else 30
    cook = [name for name in names if name in COOK_SUITES]
    items = [[name] for name in names if name not in COOK_SUITES] + ([cook] if cook else [])
    weight = lambda item: sum(seconds.get(name, typical) for name in item)  # noqa: E731
    groups = [[] for _ in range(jobs)]
    loads = [0.0] * jobs
    for item in sorted(items, key=weight, reverse=True):
        lightest = loads.index(min(loads))
        groups[lightest] += item
        loads[lightest] += weight(item)
    return [sorted(group) for group in groups if group]


def recorded_split(folder, names, jobs):
    """The worker split a results folder ran with, limited to `names`; None if it ran with another job count.

    Suites the folder did not run go to the worker with the least recorded work.
    """
    summary = json.loads((Path(folder) / 'summary.json').read_text(encoding='utf-8'))
    recorded = [entry['suites'] for entry in summary.get('workers', [])]
    if len(recorded) != jobs:
        return None
    seconds = durations(folder)
    groups = [[name for name in group if name in names] for group in recorded]
    loads = [sum(seconds.get(name, 0) for name in group) for group in groups]
    for name in names:
        if not any(name in group for group in groups):
            lightest = loads.index(min(loads))
            groups[lightest].append(name)
            loads[lightest] += seconds.get(name, 0)
    return [sorted(group) for group in groups if group]


def run_parallel_editor_suites(groups, logs):
    """Run each group of suites on its own editor, all groups at once, one sequential runner per worker mirror."""
    children = []
    for index, group in enumerate(groups):
        worker = prepare_worker(index)
        arguments = [str(PYTHON), '-B', str(worker / 'tools/run_mcp_suites.py'), '--jobs', '1', '--out', str(logs),
                     '--summary', 'worker-%d.json' % index]
        for name in group:
            arguments += ['--suite', name]
        # The owner's 16 GB budget for kept sources is split between the editors.
        environment = dict(os.environ, RELOADMAKER_RESIDENT_MB=str(RESIDENT_MEMORY_MB // len(groups)))
        children.append((worker, group, subprocess.Popen(arguments, cwd=str(worker), env=environment)))
    results = []
    workers = []
    for index, (worker, group, child) in enumerate(children):
        code = child.wait()
        summary = logs / ('worker-%d.json' % index)
        if summary.exists():
            reported = json.loads(summary.read_text(encoding='utf-8'))
            for entry in reported['results']:
                # Messages name files in the worker; report them as this workspace's paths so runs compare.
                entry['last'] = entry['last'].replace(str(worker), str(ROOT)).replace(worker.as_posix(), ROOT.as_posix())
            results += reported['results']
        else:
            last = 'Worker %d ended with code %d before reporting' % (index, code)
            results += [{'suite': name, 'status': 'blocked', 'seconds': 0, 'last': last} for name in group]
        workers.append({'root': str(worker), 'suites': group})
    return sorted(results, key=lambda entry: entry['suite']), {'workers': workers}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--suite', action='append', help='Run only these suite files')
    parser.add_argument('--out', type=Path, default=ROOT / 'local/test-results' / time.strftime('%Y%m%d-%H%M%S'))
    parser.add_argument('--baseline', type=Path,
                        help='Earlier results folder; report suites whose status or failure message changed')
    parser.add_argument('--jobs', type=int, default=MAXIMUM_JOBS,
                        help='Editors to run at once, 1 to %d. 1 runs in this workspace itself.' % MAXIMUM_JOBS)
    parser.add_argument('--rebalance', action='store_true',
                        help="Split the suites between editors from the baseline's timings instead of reusing its split")
    parser.add_argument('--summary', default='summary.json', help=argparse.SUPPRESS)
    args = parser.parse_args()
    started = time.monotonic()
    if args.baseline:
        baseline = outcomes(args.baseline)
    logs = args.out.resolve()
    logs.mkdir(parents=True, exist_ok=True)
    selected = set(args.suite or [])
    chosen = lambda names: [name for name in names if not selected or name in selected]  # noqa: E731

    results = [run_suite(name, logs) for name in chosen(OFFLINE)]
    editor = chosen(editor_suites())
    report = {}
    jobs = max(1, min(args.jobs, MAXIMUM_JOBS))
    if jobs > 1 and len(editor) > 1:
        split = None if args.rebalance or not args.baseline else recorded_split(args.baseline, editor, jobs)
        editor_results, report = run_parallel_editor_suites(split or partition(editor, jobs, durations(args.baseline)), logs)
        results += editor_results
    elif editor:
        editor_results, report = run_editor_suites(editor, logs)
        results += editor_results
    skipped = [{'suite': name, 'status': 'skipped', 'needs': needs}
               for name, needs in NEEDS_INPUTS.items() if not selected or name in selected]
    summary = {'results': results + skipped, 'wallSeconds': round(time.monotonic() - started, 1), **report}
    (logs / args.summary).write_text(json.dumps(summary, indent=1) + '\n', encoding='utf-8')
    counts = {}
    for entry in summary['results']:
        counts[entry['status']] = counts.get(entry['status'], 0) + 1
    print(json.dumps(counts), 'in %.0f s' % summary['wallSeconds'], 'logs:', logs)
    if args.baseline:
        # A refactor must not change any suite's outcome, including how the known failures fail.
        current = outcomes(logs, args.summary)
        changed = 0
        for name, (status, message) in sorted(current.items()):
            before, before_message = baseline.get(name, (None, ''))
            if before != status:
                changed += 1
                print('CHANGED %-38s %s -> %s' % (name, before, status))
            elif status != 'passed' and before_message != message:
                changed += 1
                print('MESSAGE %-38s %s' % (name, status))
                print('  before: %s' % before_message[:200])
                print('  now:    %s' % message[:200])
        print('%d suites changed against %s' % (changed, args.baseline))
        return 1 if changed else 0
    return 0 if all(entry['status'] in ('passed', 'skipped') for entry in summary['results']) else 1


def outcomes(folder, name='summary.json'):
    """Each suite's status and last output line in a results folder."""
    summary = json.loads((Path(folder) / name).read_text(encoding='utf-8'))
    return {entry['suite']: (entry['status'], entry.get('last', '')) for entry in summary['results']}


def durations(folder):
    """Seconds each suite took in an earlier run, for balancing the workers."""
    if not folder:
        return {}
    summary = json.loads((Path(folder) / 'summary.json').read_text(encoding='utf-8'))
    return {entry['suite']: entry['seconds'] for entry in summary['results'] if entry.get('seconds')}


if __name__ == '__main__':
    sys.exit(main())
