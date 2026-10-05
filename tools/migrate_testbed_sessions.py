"""One-time migration of saved Reload Maker sessions (rework P7-02, owner decision G8).

1. Magazine fit assignments and weapon defaults saved without a hand
   ("<weapon>|<magazine>", "<weapon>") are given the hand of the fit they name
   ("...|left"). A key without a hand whose hand key already exists is dropped:
   the editor never reads it.
2. The testbed sessions receive the grab points of their published packs, which
   were tested in game, where the session has none (the M1A and Timberwolf
   sessions predate picked points). The export-only inputTrack is left out.

Files are written in Qt's indented JSON, so bytes outside the change stay
identical; the script refuses to write a file it cannot reproduce exactly.
Without --apply it only reports. Delete it with the hand-agnostic fallback
(P7-04).
"""
import argparse
import configparser
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT / 'workspace'
GRIPS = WORKSPACE / 'part-grips'
VERIFIED = WORKSPACE / 'packs/verified-b5bf9bf910a249da8f180b951aa17ae1'
# Testbed session file -> published pack name in the verified pack project.
TESTBEDS = {
    'Glock19x-magazine-grips': 'Glock', 'RU556-native-reload': 'RU556', 'M1A-native-reload': 'M1A',
    'L118A2-native-reload': 'L118A2', 'Timberwolf-native-reload': 'Timberwolf', 'Thanatos-native-reload': 'Thanatos',
    'srep-service-native-reload': 'SREPService', 'srep-ar10-native-reload': 'SREPAR10',
    'srep-honeybadger-native-reload': 'SREPHoneyBadger', 'srep-xm16e1-native-reload': 'SREPXM16E1',
    'MK18-native-reload': 'MK18', 'MK18-Empire-native-reload': 'MK18Empire',
}
HANDS = ('|left', '|right')


def qt_json(value, level=0, inline_empty=True):
    """QJsonDocument::toJson(Indented): sorted keys, four spaces, raw UTF-8.
    Current Qt writes an empty object or array inline ("{}"); files from older
    builds have it on two lines."""
    pad, inner = '    ' * level, '    ' * (level + 1)
    if isinstance(value, (dict, list)) and not value and inline_empty:
        return '{}' if isinstance(value, dict) else '[]'
    if isinstance(value, dict):
        members = [inner + json.dumps(k, ensure_ascii=False) + ': ' + qt_json(value[k], level + 1, inline_empty) for k in sorted(value)]
        return '{\n' + ',\n'.join(members) + ('\n' if members else '') + pad + '}'
    if isinstance(value, list):
        items = [inner + qt_json(v, level + 1, inline_empty) for v in value]
        return '[\n' + ',\n'.join(items) + ('\n' if items else '') + pad + ']'
    return json.dumps(value, ensure_ascii=False)


def grip_hands():
    hands = {}
    for path in GRIPS.glob('*.json'):
        pose = json.loads(path.read_text(encoding='utf-8')).get('pose', {})
        if pose.get('id') and pose.get('hand') in ('left', 'right'):
            hands[pose['id']] = pose['hand']
    return hands


def explicit_hands(values, hands, report):
    """Moves every key without a hand to the hand of the fit it names."""
    changed = False
    for key in [k for k in values if not k.endswith(HANDS)]:
        hand = hands.get(values[key])
        if not hand:
            report.append('kept %s: fit %s is not in the library' % (key, values[key]))
            continue
        target = key + '|' + hand
        if target not in values:
            values[target] = values[key]
        elif values[target] != values[key]:
            report.append('dropped %s: %s already names another fit' % (key, target))
        del values[key]
        changed = True
    return changed


def published_grabs():
    grabs = {}
    for row in json.loads((VERIFIED / 'projects.json').read_text(encoding='utf-8')):
        reference = Path(row['path'])
        owner = json.loads(reference.read_text(encoding='utf-8'))['weapons'][0]
        document = json.loads((reference.parent / owner['grabs']['path']).read_text(encoding='utf-8'))
        locations = {}
        for location in document['locations']:
            key = '|'.join(location[k] for k in ('weapon', 'group', 'sourceModel', 'sourcePath')).replace('\\', '/').lower()
            locations[key] = {k: v for k, v in location.items() if k != 'inputTrack'}
        grabs[row['name']] = locations
    return grabs


def testbed_projects():
    """Session path -> published pack name: the testbeds' original session files
    and the projects the editor's settings now open for them."""
    projects = {(WORKSPACE / (stem + '.project.json')).resolve(): pack for stem, pack in TESTBEDS.items()}
    settings = configparser.ConfigParser(interpolation=None)
    settings.read(ROOT / 'local/settings/ReloadMakerV2/ReloadMakerV2.ini', encoding='utf-8')
    ids = {'glock': 'Glock', 'ru556': 'RU556', 'm1a': 'M1A', 'l118a2': 'L118A2', 'timberwolf': 'Timberwolf', 'thanatos': 'Thanatos',
           'srep-service': 'SREPService', 'srep-ar10': 'SREPAR10', 'srep-honeybadger': 'SREPHoneyBadger',
           'srep-xm16e1': 'SREPXM16E1', 'mk18': 'MK18', 'mk18-empire': 'MK18Empire'}
    if settings.has_section('testbeds'):
        for testbed, path in settings.items('testbeds'):
            if testbed in ids:
                projects[Path(path).resolve()] = ids[testbed]
    return projects


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--apply', action='store_true', help='write the migrated files')
    args = parser.parse_args()
    hands = grip_hands()
    grabs = published_grabs()
    testbeds = testbed_projects()
    files = [p for p in WORKSPACE.rglob('*.project.json') if VERIFIED.parent not in p.parents]
    files += list((ROOT / 'tests/fixtures').rglob('*.project.json'))
    failures = 0
    for path in sorted(files):
        raw = path.read_bytes()
        text = raw.decode('utf-8')
        newline = '\r\n' if '\r\n' in text else '\n'
        document = json.loads(text)
        encode = lambda value, inline: (qt_json(value, 0, inline) + '\n').replace('\n', newline).encode('utf-8')
        style = next((inline for inline in (True, False) if encode(document, inline) == raw), None)
        if style is None:
            if any(isinstance(document.get(f), dict) and any(not k.endswith(HANDS) for k in document[f])
                   for f in ('magazineGrabGrips', 'magazineGrabDefaults')):
                print('REFUSED', path.relative_to(ROOT), '(not in Qt format; migrate it by hand)')
                failures += 1
            continue
        report = []
        changed = False
        for field in ('magazineGrabGrips', 'magazineGrabDefaults'):
            if isinstance(document.get(field), dict):
                changed |= explicit_hands(document[field], hands, report)
        pack = testbeds.get(path.resolve())
        if pack:
            locations = document.setdefault('grabLocations', {})
            for key, location in grabs.get(pack, {}).items():
                if key not in locations:
                    locations[key] = location
                    report.append('imported the published grab point %s' % key)
                    changed = True
        if not changed:
            continue
        print(('MIGRATED ' if args.apply else 'WOULD MIGRATE ') + str(path.relative_to(ROOT)))
        for line in report:
            print('    ' + line)
        if args.apply:
            path.write_bytes(encode(document, style))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
