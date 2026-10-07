"""Explicit import of earlier per-weapon projects into the ESP library.

Earlier Maker versions saved one editor project per weapon and source context
(workspace/projects/<plugin>/<record>/<context>/authoring.project.json, plus
the named testbed projects) and one pack project per weapon and context
(workspace/packs/...). The source-plugin export ledger kept every group's
object IDs. This tool reads those files, never changes them, and writes:

* one ESP project per originating plugin, whose first imported version holds
  each authored weapon's chosen editor project and pack project;
* one further imported version for each competing authored project of a
  weapon, so no authoring is lost or silently merged;
* the ESP's export identity, carrying every published object ID unchanged.

A weapon's choice is read from actual identities: the project the configured
testbed names, else the project in the same source context as its published
or saved pack, else the authored project saved last. File times are recorded
as provenance only. ESP projects already in the library are left untouched.
"""
import configparser
import copy
from datetime import datetime, timezone
import glob
import json
import os
from pathlib import Path
import re

from product_library import (AUTHORING_KIND, authoring_record, commit, empty_project, get_record, ingest_pack,
                             iso, locked, new_identity, now, pin_library_records, plugin_key, project_folder,
                             put_record, read_identity, read_project, weapon_key, write_project)
from product_packs import cancellation, read, require, write

TESTBEDS = ('glock', 'ru556', 'm1a', 'timberwolf', 'l118a2', 'thanatos',
            'srep-service', 'srep-honeybadger', 'srep-ar10', 'srep-xm16e1')
CANONICAL = {'glock': 'Glock19x-magazine-grips.project.json', 'ru556': 'RU556-native-reload.project.json',
             'm1a': 'M1A-native-reload.project.json', 'l118a2': 'L118A2-native-reload.project.json',
             'thanatos': 'Thanatos-native-reload.project.json', 'timberwolf': 'Timberwolf-native-reload.project.json'}
EDITOR_PATH = re.compile(r'^([^/]+\.(?:esp|esm|esl))/([0-9a-f]{6})/([0-9a-f]{16})/authoring\.project\.json$', re.I)
PACK_PATH = re.compile(r'^([^/]+\.(?:esp|esm|esl))/([0-9a-f]{6})/([0-9a-f]{16})/pack\.project\.json$', re.I)
# Keys whose content is authoring rather than view state or source location.
AUTHORED_KEYS = ('reloadTemplates', 'authoringRoadmaps', 'grabLocations', 'magazineGrabGrips', 'operatingGrips',
                 'soundProfiles', 'scopeProfiles', 'annotations', 'magazineGripDrafts', 'weaponProperties',
                 'reloadAxes', 'magazineGrabDefaults', 'magazineRounds', 'sourcePolicies', 'gripDraft', 'mechanismDraft')
VIEW_KEYS = ('animation', 'seconds', 'motionMode', 'transitions', 'showArms', 'workspace', 'hand', 'fingerJoint',
             'selectedHandPose', 'browserPlugin', 'choices', 'recordChoices', 'camera', 'handsPage', 'interactionHand',
             'scopeSelection', 'body', 'simulationSetup', 'magazineInteraction', 'ammunitionConfiguration', 'handPreview',
             'package', 'gameData', 'library', 'mo2Instance', 'mo2Profile', 'kind')


def file_time(path):
    return datetime.fromtimestamp(Path(path).stat().st_mtime, timezone.utc).replace(microsecond=0)


def authored(document, weapon):
    if document.get('reloadTemplates', {}).get(weapon):
        return True
    return any(document.get(key) for key in AUTHORED_KEYS if key != 'reloadTemplates')


def content(document):
    return {k: v for k, v in document.items() if k not in VIEW_KEYS}


def progress_of(status):
    names = [c.get('progress') for c in status.get('configurations', {}).values() if isinstance(c, dict)]
    return 'draft' if 'draft' in names else 'complete' if 'complete' in names else 'not-started'


def settings_testbeds(workspace):
    path = workspace/'local/settings/ReloadMakerV2/ReloadMakerV2.ini'
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    if path.is_file():
        parser.read(path, encoding='utf-8')
    return {name: Path(parser.get('testbeds', name)) for name in TESTBEDS if parser.has_option('testbeds', name)}


def editor_candidates(workspace, report):
    """Every readable editor project with the context it was saved from."""
    candidates = {}
    root = workspace/'workspace/projects'
    for path in sorted(root.rglob('authoring.project.json')) if root.is_dir() else []:
        relative = path.relative_to(root).as_posix()
        match = EDITOR_PATH.match(relative)
        if not match:
            report['ignored'].append(dict(path=str(path), reason='not in the plugin/record/context layout'))
            continue
        candidates.setdefault(str(path.resolve()).lower(), dict(path=path.resolve(), context=match.group(3).lower()))
    configured = settings_testbeds(workspace)
    for name in TESTBEDS:
        for path, how in ((configured.get(name), 'configured'), (workspace/'workspace'/CANONICAL.get(name, name+'-native-reload.project.json'), 'canonical')):
            if path and path.is_file():
                entry = candidates.setdefault(str(path.resolve()).lower(), dict(path=path.resolve(), context=None))
                entry.setdefault('testbed', []).append(dict(id=name, how=how))
    result = []
    for entry in candidates.values():
        cancellation()
        try:
            document = read(entry['path'])
            require(document.get('format') == 1 and document.get('kind') == 'reload-maker-v2-project', 'not an editor project')
            weapon = weapon_key(document.get('weapon'))
        except (ValueError, OSError) as error:
            report['ignored'].append(dict(path=str(entry['path']), reason=str(error)))
            continue
        status_path = entry['path'].with_name('status.json')
        status = None
        if status_path.is_file() and entry['path'].name == 'authoring.project.json':
            try:
                status = read(status_path)
            except (ValueError, OSError):
                status = None
        result.append(dict(entry, weapon=weapon, document=document, time=file_time(entry['path']), status=status,
                           authored=authored(document, weapon)))
    return result


def published_packs(workspace, report):
    """Pack namespaces in the current source-plugin generation, by weapon."""
    published = {}
    installation = workspace/'workspace/installations/source-esl'
    current = installation/'current.json'
    if not current.is_file():
        return published, None
    generation = installation/read(current)['generation']
    ledger = read(generation/'ledger.json')
    for manifest in glob.glob(str(generation/'output/F4SE/Plugins/DIAMOND/Packs/*/manifest.json')):
        data = read(Path(manifest))
        for weapon in data['weapons']:
            published[weapon_key(weapon['record'])] = dict(id=data.get('authoringId', data['id']), revision=data['revision'])
    report['ledger'] = dict(path=str(generation/'ledger.json'), groups=sorted(ledger.get('groups', {})))
    return published, ledger


def pack_candidates(workspace):
    result = []
    root = workspace/'workspace/packs'
    for path in sorted(root.rglob('pack.project.json')) if root.is_dir() else []:
        try:
            data = read(path)
            require(data.get('format') == 'reload-maker-pack-project', 'not a pack project')
        except (ValueError, OSError):
            continue
        match = PACK_PATH.match(path.relative_to(root).as_posix())
        for weapon in data.get('weapons', []):
            try:
                record = weapon_key(weapon.get('record'))
            except ValueError:
                continue
            result.append(dict(path=path.resolve(), id=data['id'], revision=data.get('revision', 0), weapon=record,
                               context=match.group(3).lower() if match else None, time=file_time(path)))
    return result


def choose_pack(weapon, packs, published, context):
    owned = [p for p in packs if p['weapon'] == weapon]
    if weapon in published:
        same = [p for p in owned if p['id'] == published[weapon]['id']]
        if same:
            return max(same, key=lambda p: (p['revision'], p['time']))
    default = [p for p in owned if p['context'] is not None]
    if context:
        inside = [p for p in default if p['context'] == context]
        if inside:
            return max(inside, key=lambda p: (p['revision'], p['time']))
    return max(default, key=lambda p: (p['revision'], p['time'])) if default else None


def choose_editor(weapon, candidates, pack):
    owned = [c for c in candidates if c['weapon'] == weapon and c['authored']]
    if not owned:
        return None, []
    configured = [c for c in owned if any(t['how'] == 'configured' for t in c.get('testbed', []))]
    canonical = [c for c in owned if c.get('testbed')]
    templated = [c for c in owned if c['document'].get('reloadTemplates', {}).get(weapon)]
    if configured:
        primary = configured[0]
    elif pack and pack['context'] and any(c['context'] == pack['context'] for c in owned):
        primary = max((c for c in owned if c['context'] == pack['context']), key=lambda c: c['time'])
    elif canonical:
        primary = max(canonical, key=lambda c: c['time'])
    else:
        primary = max(templated or owned, key=lambda c: c['time'])
    alternatives, seen = [], [content(primary['document'])]
    for candidate in sorted(owned, key=lambda c: c['time']):
        if candidate is primary or content(candidate['document']) in seen:
            continue
        seen.append(content(candidate['document']))
        alternatives.append(candidate)
    return primary, alternatives


def weapon_entry(folder, workspace, candidate, pack_reference, library):
    authoring, source = authoring_record(candidate['document'])
    document = candidate['document']
    entry = dict(name=document.get('weaponName') or candidate['weapon'], authoring=put_record(folder, authoring),
                 source=source)
    if document.get('weaponEditorId'):
        entry['editorId'] = document['weaponEditorId']
    if candidate['status']:
        entry['status'] = dict(candidate['status'], progress=progress_of(candidate['status']))
    if pack_reference:
        entry['pack'] = pack_reference
    pin_library_records(folder, workspace, [authoring], library)
    return entry


def provenance_of(candidate, pack):
    value = dict(editorProject=str(candidate['path']), editorSavedAt=iso(candidate['time']),
                 timestampReliable=False, context=candidate['context'])
    if candidate.get('testbed'):
        value['testbed'] = candidate['testbed']
    if pack:
        value.update(packProject=str(pack['path']), packRevision=pack['revision'], packSavedAt=iso(pack['time']))
    return value


def development_sources(workspace, candidates, chosen):
    """Where the developer's testbeds and dev bench weapons load from. This
    names sources only; it never supplies authoring."""
    bench = str((workspace/'local/devbench').resolve()).replace('\\', '/').lower()+'/'
    entries = {}
    for candidate in candidates:
        document = candidate['document']
        if document.get('library') or not document.get('package'):
            continue
        source = str(document['package'])
        weapon = document.get('weapon', candidate['weapon'])
        for testbed in candidate.get('testbed', []):
            if testbed['how'] == 'configured' or testbed['id'] not in entries:
                entries[testbed['id']] = dict(id=testbed['id'], weapon=weapon, source=source, gameData=document.get('gameData', ''),
                                              plugin=document.get('browserPlugin', weapon.split(':')[0]),
                                              name=document.get('weaponName', testbed['id']))
        if not bench_source(source, bench):
            continue
        # A dev bench weapon loads from the source its chosen project used; a
        # weapon with only drafts keeps the newest draft's source.
        slug = re.sub(r'[^a-z0-9]+', '-', Path(weapon.split(':')[0]).stem.lower()+'-'+weapon.split(':')[1].lower())
        rank = (chosen.get(candidate['weapon']) is candidate, candidate['authored'], candidate['time'])
        if 'bench-'+slug in entries and entries['bench-'+slug]['rank'] >= rank:
            continue
        entries['bench-'+slug] = dict(id='bench-'+slug, weapon=weapon, source=source, bench=True, gameData=document.get('gameData', ''),
                                      plugin=weapon.split(':')[0], name=document.get('weaponName', weapon), rank=rank)
    for entry in entries.values():
        entry.pop('rank', None)
    return sorted(entries.values(), key=lambda e: (e.get('bench', False), e['id']))


def bench_source(source, bench):
    """A package inside the dev bench, or a source set naming one."""
    normalized = source.replace('\\', '/').lower()
    if normalized.startswith(bench):
        return True
    if not normalized.endswith('.reloadsources.json') or not Path(source).is_file():
        return False
    try:
        definition = read(Path(source), 64*1024)
    except (ValueError, OSError):
        return False
    folder = Path(source).parent
    return any(str((folder/str(p)).resolve()).replace('\\', '/').lower().startswith(bench) for p in definition.get('plugins', []))


def migrate(workspace, apply=False):
    workspace = Path(workspace).resolve()
    report = dict(ok=True, applied=apply, ignored=[], projects=[], skippedProjects=[], conflicts=[], missing=[])
    candidates = editor_candidates(workspace, report)
    published, ledger = published_packs(workspace, report)
    packs = pack_candidates(workspace)
    weapons = sorted({c['weapon'] for c in candidates if c['authored']} | set(published))
    plan = {}
    chosen = {}
    for weapon in weapons:
        cancellation()
        primary_context = None
        pack = choose_pack(weapon, packs, published, None)
        primary, alternatives = choose_editor(weapon, candidates, pack)
        if primary:
            chosen[weapon] = primary
            primary_context = primary['context']
            preferred = choose_pack(weapon, packs, published, primary_context)
            pack = preferred or pack
        if weapon in published and (not pack or pack['id'] != published[weapon]['id']):
            report['missing'].append(dict(weapon=weapon, reason='published pack project not found', published=published[weapon]))
        if not primary and not pack:
            continue
        plan.setdefault(weapon.split(':')[0], []).append(dict(weapon=weapon, primary=primary, alternatives=alternatives, pack=pack))
        blanks = [str(c['path']) for c in candidates if c['weapon'] == weapon and not c['authored']]
        if alternatives:
            report['conflicts'].append(dict(weapon=weapon, chosen=str(primary['path']), alternatives=[str(a['path']) for a in alternatives]))
        if blanks:
            report['ignored'].extend(dict(path=p, reason='blank draft without authoring') for p in blanks)
    planned = {i['weapon'] for items in plan.values() for i in items}
    for candidate in candidates:
        if candidate['weapon'] not in planned:
            report['ignored'].append(dict(path=str(candidate['path']), reason='blank draft without authoring'))
    for key, items in sorted(plan.items()):
        cancellation()
        plugin = next((i['primary']['document'].get('weapon', '').split(':')[0] for i in items if i['primary']), key)
        folder = project_folder(workspace, plugin)
        row = dict(plugin=plugin, key=key, weapons=[dict(weapon=i['weapon'], editor=str(i['primary']['path']) if i['primary'] else None,
                                                         pack=str(i['pack']['path']) if i['pack'] else None,
                                                         alternatives=len(i['alternatives'])) for i in items])
        if read_project(folder) is not None:
            report['skippedProjects'].append(dict(plugin=plugin, reason='already in the library; left unchanged'))
            continue
        report['projects'].append(row)
        if not apply:
            continue
        with locked(folder):
            entries, library, provenance, problems = {}, {}, {}, []
            for item in items:
                pack_reference = None
                if item['pack']:
                    try:
                        pack_reference = ingest_pack(folder, item['pack']['path'])
                    except (ValueError, OSError) as error:
                        problems.append(dict(weapon=item['weapon'], error='pack not imported: '+str(error)))
                if item['primary']:
                    entries[item['weapon']] = weapon_entry(folder, workspace, item['primary'], pack_reference, library)
                    provenance[item['weapon']] = provenance_of(item['primary'], item['pack'])
                elif pack_reference:
                    entries[item['weapon']] = dict(name=item['weapon'], pack=pack_reference)
                    provenance[item['weapon']] = dict(packProject=str(item['pack']['path']), editorProject=None)
            row['problems'] = problems
            identity_value = read_identity(folder) or new_identity(plugin)
            for item in items:
                if item['pack']:
                    identity_value['packIds'][item['weapon']] = item['pack']['id']
            if ledger and key in ledger.get('groups', {}):
                group = copy.deepcopy(ledger['groups'][key])
                group.pop('generation', None)
                identity_value['group'] = group
                identity_value['installation'] = ledger['installation']
            moment = now()
            # Competing projects first, so the chosen import is the latest.
            for item in items:
                for alternative in item['alternatives']:
                    # An alternative keeps only a pack saved from its own source
                    # context; another context's behavior would not match it.
                    own = next((p for p in packs if p['weapon'] == item['weapon'] and p['context'] and
                                p['context'] == alternative['context']), None)
                    own_reference = None
                    if own:
                        try:
                            own_reference = ingest_pack(folder, own['path'])
                        except (ValueError, OSError) as error:
                            problems.append(dict(weapon=item['weapon'], error='alternative pack not imported: '+str(error)))
                    variant = dict(entries)
                    variant[item['weapon']] = weapon_entry(folder, workspace, alternative, own_reference, library)
                    commit(folder, plugin, variant, dict(library), origin='migration',
                           note='Imported alternative for '+variant[item['weapon']]['name'], moment=moment,
                           provenance=dict(provenance, **{item['weapon']: provenance_of(alternative, own)}))
            info = commit(folder, plugin, entries, library, origin='migration', note='Imported from earlier projects',
                          moment=moment, provenance=provenance)
            write(folder/'export/identity.json', identity_value)
            row['save'] = info['id']
    if apply:
        registry = workspace/'workspace/library/development-sources.json'
        if not registry.exists():
            write(registry, dict(format='reload-maker-development-sources', version=1,
                                 sources=development_sources(workspace, candidates, chosen)))
            report['developmentSources'] = str(registry)
    return report
