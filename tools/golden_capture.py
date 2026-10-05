"""Record and compare Reload Maker golden outputs for the testbed corpus.

capture  Launch Maker, load every testbed through the MCP command pipe and record
         its exports, read-only states and two native renders. Session files the
         editor rewrites while switching testbeds are restored afterwards, so a
         capture leaves the authoring corpus byte-identical.
compare  Compare two captures. JSON must match exactly after canonical formatting;
         renders must match within a small per-channel tolerance.

The behavior-preserving refactor gates every step on `compare` reporting no
differences against the baseline recorded before the refactor started.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

# The bundled runtime is isolated and does not add the script folder to the path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from editor_session import ROOT, TESTBEDS, EditorRun, command, edit, ready

# Read-only state commands recorded for every testbed.
STATES = (
    'checklist_state', 'operating_setup_state', 'magazine_guide_state', 'magazine_setup_state',
    'magazine_reference_state', 'source_policy_state', 'sound_state',
    'grab_locations_state', 'simulation_state', 'source_browser', 'installation', 'home',
)
EXPORTS = ('runtime_configuration', 'sounds', 'grab_locations', 'simulation', 'reload_bundle')
VIEWS = ('side', 'iso')

# Fields added by the command server or describing the live process, not the output.
VOLATILE_KEYS = {'revision', 'documentRevision', 'session', 'camera', 'status', 'busy', 'loading', 'capturing'}

# Other receivers sampled per testbed, besides the receiver its session selects.
SAMPLED_RECEIVERS = 2
RENDER_TOLERANCE = 8


def attempt(function):
    """Record a value or the editor's error text; both are part of the output."""
    try:
        return {'ok': function()}
    except Exception as error:  # the editor reports rejections as RuntimeError text
        return {'error': str(error)}


# ---------------------------------------------------------------- canonical output

def canonical(value):
    if isinstance(value, dict):
        return {key: canonical(item) for key, item in value.items() if key not in VOLATILE_KEYS}
    if isinstance(value, list):
        return [canonical(item) for item in value]
    return value


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(canonical(value), indent=1, sort_keys=True, ensure_ascii=False) + '\n', encoding='utf-8')


def export(kind):
    result = edit('export', {'kind': kind})
    return json.loads(Path(result['path']).read_text(encoding='utf-8'))


def find_slot(state, name):
    return next((slot for slot in state['slots'] if slot['name'] == name), None)


def select(slot_name, mod_id):
    """Select one part. Slot keys are re-read because a receiver change rebuilds the slots."""
    slot = find_slot(ready(), slot_name)
    if slot is None:
        raise RuntimeError('No %s slot in this configuration' % slot_name)
    # The editor rejects re-selecting the current part as an invalid change.
    if slot['selected'] != mod_id:
        edit('configure', {'slot': slot['key'], 'mod': mod_id})
        ready()


# ---------------------------------------------------------------- capture

def capture_testbed(name, folder):
    edit('testbed', {'name': name})
    state = ready()
    if state['lastError']:
        write_json(folder / 'load-error.json', {'error': state['lastError']})
        return
    weapon = state['weapons'][state['weaponIndex']] if state['weaponIndex'] >= 0 else None
    write_json(folder / 'state.json', {
        'weapon': weapon,
        'slots': state['slots'],
        'actions': state['actions'],
        'anchors': state['anchors'],
        'interaction': state['interaction'],
        'magazineGrabGrips': state['magazineGrabGrips'],
        'workspace': state['workspace'],
    })
    for name_of_state in STATES:
        write_json(folder / 'states' / (name_of_state + '.json'), attempt(lambda: command(name_of_state)))
    for kind in EXPORTS:
        write_json(folder / 'exports' / (kind + '.json'), attempt(lambda: export(kind)))
    for view in VIEWS:
        rendered = attempt(lambda: edit('render', {'view': view, 'fit': True}))
        if 'ok' in rendered:
            shutil.copyfile(rendered['ok']['path'], folder / ('render-' + view + '.png'))
        else:
            write_json(folder / ('render-' + view + '-error.json'), rendered)
    capture_configurations(folder)


def capture_configurations(folder):
    """Runtime export for every magazine on the saved receiver, plus sampled receivers."""
    state = ready()
    magazines = find_slot(state, 'Magazine')
    receivers = find_slot(state, 'Receiver')
    if not magazines:
        write_json(folder / 'configurations.json', {'error': 'No Magazine slot'})
        return
    results = {}

    def configured_export(slot_name, mod_id):
        select(slot_name, mod_id)
        return export('runtime_configuration')

    for choice in magazines['choices']:
        key = 'magazine:' + choice['editorId']
        results[key] = attempt(lambda: configured_export('Magazine', choice['id']))
    results['restore:magazine'] = attempt(lambda: select('Magazine', magazines['selected']))
    if receivers:
        others = [choice for choice in receivers['choices'] if choice['id'] != receivers['selected']]
        for choice in others[:SAMPLED_RECEIVERS]:
            key = 'receiver:' + choice['editorId']
            results[key] = attempt(lambda: configured_export('Receiver', choice['id']))
        results['restore:receiver'] = attempt(lambda: select('Receiver', receivers['selected']))
    write_json(folder / 'configurations.json', results)


def capture(out, names):
    out.mkdir(parents=True, exist_ok=True)
    head = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    failures = {}
    with EditorRun() as run:
        for name in names:
            print('capturing', name, flush=True)
            try:
                capture_testbed(name, out / name)
            except Exception as error:
                failures[name] = str(error)
                print('  failed:', error, flush=True)
    manifest = {'commit': head, 'testbeds': list(names), 'failures': failures, **run.report()}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n', encoding='utf-8')
    print(json.dumps(manifest, indent=1))
    return 1 if failures else 0


# ---------------------------------------------------------------- compare

def render_difference(a, b):
    from PIL import Image, ImageChops
    first = Image.open(a).convert('RGBA')
    second = Image.open(b).convert('RGBA')
    if first.size != second.size:
        return 'size %s != %s' % (first.size, second.size)
    largest = max(high for _, high in ImageChops.difference(first, second).getextrema())
    return None if largest <= RENDER_TOLERANCE else 'max channel difference %d' % largest


def compare(a, b):
    differences = []
    files = sorted({path.relative_to(a) for path in a.rglob('*') if path.is_file()} |
                   {path.relative_to(b) for path in b.rglob('*') if path.is_file()})
    for relative in files:
        if relative.name == 'manifest.json':
            continue
        first, second = a / relative, b / relative
        if not first.exists() or not second.exists():
            differences.append('%s: only in %s' % (relative, a if first.exists() else b))
        elif relative.suffix == '.png':
            problem = render_difference(first, second)
            if problem:
                differences.append('%s: %s' % (relative, problem))
        elif hashlib.sha256(first.read_bytes()).digest() != hashlib.sha256(second.read_bytes()).digest():
            differences.append('%s: content differs' % relative)
    for line in differences:
        print(line)
    print('%d differences in %d files' % (len(differences), len(files)))
    return 1 if differences else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    record = commands.add_parser('capture')
    record.add_argument('out', type=Path)
    record.add_argument('--testbed', action='append', choices=TESTBEDS)
    check = commands.add_parser('compare')
    check.add_argument('baseline', type=Path)
    check.add_argument('candidate', type=Path)
    args = parser.parse_args()
    if args.command == 'capture':
        return capture(args.out.resolve(), tuple(args.testbed or TESTBEDS))
    return compare(args.baseline.resolve(), args.candidate.resolve())


if __name__ == '__main__':
    sys.exit(main())
