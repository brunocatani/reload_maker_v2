"""Fails when application source names a particular weapon (rework 14, item 26).

Maker reads a weapon's behavior from its preset and detected properties, never
from which weapon it is. This check scans the string literals in src/ for the
development weapons' editor IDs, plugin files and animation folders, the names
the old per-weapon branches used. The developer testbeds are named only in
workspace/library/development-sources.json, so no source file is exempt.

Usage: check_weapon_names.py <src folder>
"""
import re
import sys
from pathlib import Path

# Editor IDs, plugin files and animation folders of the development weapons.
NAMES = re.compile(
    r'wpnglock19x|wpnru556|wpnmk18|wpnm1a|srep_|diam107|c14timberwolf|l118a2'
    r'|glock19x\.esp|ru556\.esp|m1a\.esp|mk18\.esp|mk18attachmentoverhaul\.esp|1cas_servicerifle\.esp|diarifles\.esp'
    r'|glock19xanims|ru556anims|mk18anims|m1aanims|c14timb|srep/|l118/',
    re.IGNORECASE)
LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"')
EXEMPT = set()


def code(line):
    """The line without a trailing // comment outside string literals."""
    quoted = False
    for i, c in enumerate(line):
        if c == '"' and (i == 0 or line[i - 1] != '\\'):
            quoted = not quoted
        elif not quoted and line.startswith('//', i):
            return line[:i]
    return line


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    root = Path(argv[1])
    found = []
    for path in sorted(root.rglob('*')):
        if path.suffix not in ('.cpp', '.h') or path.relative_to(root).as_posix() in EXEMPT:
            continue
        for number, line in enumerate(path.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
            for literal in LITERAL.findall(code(line)):
                if NAMES.search(literal):
                    found.append('%s:%d: %s' % (path.relative_to(root).as_posix(), number, literal))
    for entry in found:
        print(entry)
    print('FAIL %d weapon-specific literals' % len(found) if found else 'PASS no weapon editor IDs, plugins or animation folders in src/')
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
