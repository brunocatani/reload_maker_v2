"""One-time migration of the testbed sessions (rework G8, backlog P7-02).

Before the weapon properties existed, Maker chose each testbed's behavior by
its editor ID. This script writes that choice into the sessions instead: the
behavior preset each testbed ran with, and the per-weapon properties where
the testbed differed from its preset's default. After it has run, the editor
no longer needs to recognize any weapon by name.

It also adds the operating part's model to operating finger keys
("weapon|group|hand" becomes "weapon|group|model|hand"); only the MK18 and
SREP keys carried the model before (rework 14, item 20).

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

# The model of each testbed's operating part (its receiver's), read from the
# editor at 0d92784. Thanatos cannot load here (DLCRobot.esm is missing), so
# its key stays as it is until its model is known.
OPERATING_MODELS = {
    'glock19x.esp:000f9b|slide': 'meshes/weapons/glock19x/glock19x/glock19x.nif',
    'ru556.esp:000f99|charging_handle': 'meshes/weapons/ru556/ru556/ru556_receiver.nif',
    'm1a.esp:000f9a|operating_handle': 'meshes/weapons/m1a_404/m1a/m1a_receiver.nif',
    'l118a2.esp:00206c|bolt': 'meshes/weapons/timberwolf/timberwolfreceiver.nif',
    'l118a2.esp:000002|bolt': 'meshes/weapons/l118/l118a2receiver.nif',
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


def scope_operating_grips(session, changes):
    grips = session.get('operatingGrips', {})
    for key in list(grips):
        parts = key.split('|')
        if len(parts) != 3:
            continue
        model = OPERATING_MODELS.get('|'.join(parts[:2]))
        if not model:
            changes.append('kept unscoped ' + key)
            continue
        scoped = '|'.join(parts[:2] + [model, parts[2]])
        value = grips.pop(key)
        grips.setdefault(scoped, value)
        changes.append('scoped ' + key)


def migrate(path):
    session = json.loads(path.read_text(encoding='utf-8'))
    editor_id = session.get('weaponEditorId', '').lower()
    weapon = session.get('weapon', '').lower()
    changes = []
    if editor_id in PRESETS and weapon:
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
    scope_operating_grips(session, changes)
    if not [c for c in changes if not c.startswith('kept')]:
        return ', '.join(changes) or 'unchanged'
    path.write_text(json.dumps(session, indent=4, sort_keys=True, ensure_ascii=False) + '\n', encoding='utf-8')
    return ', '.join(changes)


def main():
    for path in sessions():
        print('%s: %s' % (path, migrate(path)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
