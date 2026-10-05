"""One-time migration of the testbed sessions (rework G8, backlog P7-02).

Before the weapon properties existed, Maker chose each testbed's behavior by
its editor ID. This script writes that choice into the sessions instead: the
behavior preset each testbed ran with, and the per-weapon properties where
the testbed differed from its preset's default. After it has run, the editor
no longer needs to recognize any weapon by name.

It edits, in place, every session the testbed switcher can open: the paths
saved in the settings file and the default projects in workspace/. Running it
again changes nothing.
"""
import configparser
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SETTINGS = ROOT / 'local/settings/ReloadMakerV2/ReloadMakerV2.ini'
DEFAULT_PROJECTS = (
    'Glock19x-magazine-grips', 'RU556-native-reload', 'M1A-native-reload', 'Timberwolf-native-reload',
    'L118A2-native-reload', 'Thanatos-native-reload', 'MK18-native-reload', 'MK18-Empire-native-reload',
    'srep-service-native-reload', 'srep-ar10-native-reload', 'srep-honeybadger-native-reload', 'srep-xm16e1-native-reload',
)

PISTOL = 'pistol.slide-detachable.v1'
AR15 = 'rifle.ar15-detachable.v1'
OPERATING_HANDLE = 'rifle.operating-handle-rocking.v1'
RECIPROCATING = 'rifle.reciprocating-detachable.v1'
BOLT_ACTION = 'sniper.manual-bolt-detachable.v1'

# The mapping the editor applied by editor ID until the weapon properties
# replaced it (MechanismIdentity.cpp at 6a8133e).
PRESETS = {
    'wpnglock19x': PISTOL,
    'wpnru556': AR15,
    'wpnmk18': AR15,
    'wpnmk18unique': AR15,
    'srep_servicerifle': AR15,
    'srep_servicerifle_unique_honeybadger': AR15,
    'srep_xm16e1': AR15,
    'srep_ar10': RECIPROCATING,
    'diam107': RECIPROCATING,
    'wpnm1a': OPERATING_HANDLE,
    'c14timberwolf': BOLT_ACTION,
    'l118a2': BOLT_ACTION,
}

# Testbeds whose behavior differed from their preset's default.
PROPERTIES = {
    # The Thanatos magazine followed its clip's insertion curve.
    'diam107': {'insertion': 'native-curve'},
}


def sessions():
    settings = configparser.ConfigParser(interpolation=None)
    settings.optionxform = str
    settings.read(SETTINGS, encoding='utf-8')
    paths = []
    if settings.has_section('testbeds'):
        paths += [Path(value) for _, value in settings.items('testbeds')]
    paths += [ROOT / 'workspace' / (name + '.project.json') for name in DEFAULT_PROJECTS]
    unique = []
    for path in paths:
        if path.exists() and path.resolve() not in [p.resolve() for p in unique]:
            unique.append(path)
    return unique


def migrate(path):
    session = json.loads(path.read_text(encoding='utf-8'))
    editor_id = session.get('weaponEditorId', '').lower()
    weapon = session.get('weapon', '').lower()
    if editor_id not in PRESETS or not weapon:
        return 'skipped (%s)' % (editor_id or 'no weapon')
    changes = []
    templates = session.setdefault('reloadTemplates', {})
    if templates.get(weapon) != PRESETS[editor_id]:
        templates[weapon] = PRESETS[editor_id]
        changes.append('preset ' + PRESETS[editor_id])
    wanted = PROPERTIES.get(editor_id)
    if wanted:
        properties = session.setdefault('weaponProperties', {})
        current = properties.get(weapon, {})
        if any(current.get(key) != value for key, value in wanted.items()):
            properties[weapon] = {**current, **wanted}
            changes.append('properties %s' % wanted)
    if not changes:
        return 'unchanged'
    path.write_text(json.dumps(session, indent=4, sort_keys=True, ensure_ascii=False) + '\n', encoding='utf-8')
    return ', '.join(changes)


def main():
    for path in sessions():
        print('%s: %s' % (path, migrate(path)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
