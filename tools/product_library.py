"""The ESP-owned project library: one authoring project per originating plugin.

Every authored weapon belongs to the plugin that defines its record, so the
project key is that plugin's lower-case filename. Where the source was loaded
from (a testbed, a development package, MO2) is metadata, never an identity.

    workspace/library/plugins/<key>/
        project.json              display name, committed versions, latest
        saves/<id>/snapshot.json  one committed version of the whole ESP
        records/<sha256>.json     JSON content: weapon authoring, behavior
                                  documents, pack projects, pinned poses
        assets/<sha256><ext>      finished files: magazine models and their
                                  cook receipts, weapon model overrides
        export/identity.json      permanent pack namespaces and object IDs
        export/published.json     where each version was exported

Content names only address files. Nothing is refused, invalidated or skipped
because a digest differs (owner rule, 2026-10-05).

Only a deliberate save writes a version. A save stages its snapshot folder,
moves it into place and replaces project.json last, so a failed or cancelled
save leaves the previous latest version and every earlier one intact. Records
and assets are shared between versions; nothing removes them as a side effect.
The export identity lives outside the versions: loading or deleting a version
never touches it.
"""
import contextlib
import copy
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import uuid

from product_packs import cancellation, identity, read, require, write

PROJECT_FORMAT = 'reload-maker-esp-project'
SNAPSHOT_FORMAT = 'reload-maker-esp-snapshot'
AUTHORING_KIND = 'reload-maker-v2-weapon-authoring'
LIBRARY_FOLDERS = {'handPoses': 'hand-poses', 'partGrips': 'part-grips', 'mechanisms': 'part-mechanisms'}
PLUGIN_NAME = re.compile(r'[^:/\\\x00-\x1f<>"|?*]+\.(?:esm|esp|esl)', re.I)
RECORD_KEY = re.compile(r'[^:/\\\x00]+\.(?:esm|esp|esl):[0-9a-f]{6}', re.I)
UUID = re.compile(r'[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}', re.I)
SAVE_ID = re.compile(r'\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}Z_\d{4,}')
REFERENCE = re.compile(r'(?:records/[0-9a-f]{64}\.json|assets/[0-9a-f]{64}\.[a-z0-9]{1,8})')
# The source-location fields of an editor project record. A version keeps them
# as metadata of the weapon; they never select the source to load.
SOURCE_FIELDS = ('package', 'gameData', 'library', 'mo2Instance', 'mo2Profile')


def plugin_key(plugin):
    require(isinstance(plugin, str) and PLUGIN_NAME.fullmatch(plugin) is not None, 'Invalid source plugin name')
    return plugin.lower()


def weapon_key(record):
    require(isinstance(record, str) and RECORD_KEY.fullmatch(record) is not None,
            'Expected an originating plugin and six-digit local record ID')
    return record.lower()


def library_root(workspace):
    return Path(workspace).resolve()/'workspace/library/plugins'


def project_folder(workspace, plugin):
    return library_root(workspace)/plugin_key(plugin)


def now():
    return datetime.now(timezone.utc).replace(microsecond=0)


def iso(moment):
    return moment.astimezone(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def save_id(moment, sequence):
    return moment.astimezone(timezone.utc).strftime('%Y-%m-%dT%H-%M-%SZ')+f'_{sequence:04d}'


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False, allow_nan=False).encode('utf-8')


def resolve(folder, reference):
    require(isinstance(reference, str) and REFERENCE.fullmatch(reference) is not None, 'Invalid library reference')
    return folder/reference


@contextlib.contextmanager
def locked(folder):
    """One writer per ESP project. The operating system releases the lock when
    a crashed writer exits, so a stale lock never blocks the next save."""
    folder.mkdir(parents=True, exist_ok=True)
    handle = open(folder/'.lock', 'a+b')
    try:
        try:
            if os.name == 'nt':
                import msvcrt
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise ValueError('Another save, delete or export of this ESP project is running') from None
        yield
    finally:
        handle.close()


# ---- Content-named records and assets ----

def put_record(folder, value):
    data = canonical(value)
    name = 'records/'+hashlib.sha256(data).hexdigest()+'.json'
    target = folder/name
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        pending = target.with_name(target.name+'.'+uuid.uuid4().hex+'.pending')
        pending.write_bytes(data)
        os.replace(pending, target)
    return name


def get_record(folder, reference, limit=256*1024*1024):
    return read(resolve(folder, reference), limit)


def put_asset(folder, source):
    """Copies a finished file under its content name. A cooked magazine keeps
    its cook receipt beside it under the same name."""
    source = Path(source).resolve()
    require(source.is_file(), f'Missing finished file: {source}')
    with source.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    suffix = source.suffix.lower()
    require(re.fullmatch(r'\.[a-z0-9]{1,8}', suffix) is not None, f'Unsupported finished file type: {source}')
    name = 'assets/'+digest+suffix
    target = folder/name
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        pending = target.with_name(target.name+'.'+uuid.uuid4().hex+'.pending')
        shutil.copy2(source, pending)
        os.replace(pending, target)
    receipt = source.with_suffix('.receipt.json')
    owned = target.with_suffix('.receipt.json')
    if suffix == '.nif' and receipt.is_file() and not owned.exists():
        pending = owned.with_name(owned.name+'.'+uuid.uuid4().hex+'.pending')
        shutil.copy2(receipt, pending)
        os.replace(pending, owned)
    return name


# ---- Project index ----

def empty_project(plugin):
    return dict(format=PROJECT_FORMAT, version=1, plugin=plugin, key=plugin_key(plugin), nextSequence=1, latest=None, saves=[])


def read_project(folder):
    path = folder/'project.json'
    if not path.is_file():
        return None
    project = read(path, 32*1024*1024)
    require(project.get('format') == PROJECT_FORMAT and project.get('version') == 1, 'Unsupported ESP project index: '+str(path))
    require(project.get('key') == folder.name, 'ESP project index belongs to another plugin: '+str(path))
    require(isinstance(project.get('saves'), list), 'Invalid saved version list: '+str(path))
    return project


def latest_save(project):
    """The newest completed save in recorded save order, never by file time."""
    saves = sorted(project['saves'], key=lambda s: s['sequence'])
    return saves[-1]['id'] if saves else None


def write_project(folder, project):
    project['saves'] = sorted(project['saves'], key=lambda s: s['sequence'])
    project['latest'] = latest_save(project)
    write(folder/'project.json', project)


def read_snapshot(folder, save):
    require(isinstance(save, str) and SAVE_ID.fullmatch(save) is not None, 'Invalid saved version')
    snapshot = read(folder/'saves'/save/'snapshot.json', 32*1024*1024)
    require(snapshot.get('format') == SNAPSHOT_FORMAT and snapshot.get('version') == 1 and snapshot.get('id') == save,
            'Unsupported or damaged saved version: '+save)
    return snapshot


def list_projects(workspace):
    root = library_root(workspace)
    result = []
    if root.is_dir():
        for folder in sorted(p for p in root.iterdir() if p.is_dir() and not p.name.startswith('.')):
            row = dict(key=folder.name)
            try:
                project = read_project(folder)
                if project is None:
                    continue
                row.update(plugin=project['plugin'], latest=project['latest'], saves=project['saves'],
                           published=read_published(folder))
            except (ValueError, OSError) as error:
                row['error'] = str(error)
            result.append(row)
    return result


# ---- Export identity ----

def read_identity(folder):
    path = folder/'export/identity.json'
    if not path.is_file():
        return None
    value = read(path, 32*1024*1024)
    require(value.get('format') == 'reload-maker-esp-export-identity' and value.get('version') == 1,
            'Unsupported ESP export identity: '+str(path))
    return value


def new_identity(plugin):
    return dict(format='reload-maker-esp-export-identity', version=1, plugin=plugin_key(plugin),
                installation=str(uuid.uuid4()), packIds={}, group=None)


def read_published(folder):
    path = folder/'export/published.json'
    return read(path, 8*1024*1024) if path.is_file() else dict(format='reload-maker-esp-publications', version=1, publications=[])


def pack_namespace(plugin, record, identity_value, existing=None):
    """A weapon's pack namespace names its logical magazine objects, so it is
    permanent: the identity keeps it even when every version is deleted."""
    record = weapon_key(record)
    known = identity_value['packIds'].get(record) or existing
    if known:
        return identity(known)
    slug = re.sub(r'[^a-z0-9_-]+', '-', Path(plugin).stem.lower()).strip('-')[:48] or 'plugin'
    return identity('reload.'+slug+'.'+record.split(':')[1])


# ---- Pack projects inside the library ----

def ingest_pack(folder, project_path):
    """Stores a pack project and everything it references as library content.
    Returns the stored pack record's reference."""
    project_path = Path(project_path).resolve()
    project = read(project_path)
    require(project.get('format') == 'reload-maker-pack-project', 'Not a pack project: '+str(project_path))
    project = copy.deepcopy(project)

    def located(entry):
        path = Path(entry['path'])
        return (path if path.is_absolute() else project_path.parent/path).resolve()

    for weapon in project['weapons']:
        for kind in ('profiles', 'magazines', 'sounds', 'grabs'):
            if kind in weapon:
                # Behavior documents keep their exact bytes, so an unchanged
                # weapon exports byte-identical files.
                path = located(weapon[kind])
                read(path)
                weapon[kind] = dict(path=put_asset(folder, path))
    for obj in project['objects']:
        obj['source'] = dict(path=put_asset(folder, located(obj['source'])))
    for entry in project.get('modelOverrides', []):
        entry['path'] = put_asset(folder, located(entry))
    return put_record(folder, project)


def materialize_pack(folder, reference, destination):
    """Writes a pack project whose inputs are the library's own files, for the
    existing authoring and freezing tools."""
    project = get_record(folder, reference)
    for weapon in project['weapons']:
        for kind in ('profiles', 'magazines', 'sounds', 'grabs'):
            if kind in weapon:
                weapon[kind] = dict(path=str(resolve(folder, weapon[kind]['path'])))
    for obj in project['objects']:
        obj['source'] = dict(path=str(resolve(folder, obj['source']['path'])))
    for entry in project.get('modelOverrides', []):
        entry['path'] = str(resolve(folder, entry['path']))
    destination = Path(destination)
    write(destination, project)
    return destination


# ---- Pinned shared poses ----

def pin_library_records(folder, workspace, values, pinned):
    """Records each shared hand pose, part grip or mechanism a weapon's
    authoring names, so a later library edit cannot change this version."""
    root = Path(workspace).resolve()/'workspace'
    queue = []

    def scan(value):
        if isinstance(value, str):
            for match in UUID.findall(value):
                queue.append(match.lower())
        elif isinstance(value, list):
            for item in value:
                scan(item)
        elif isinstance(value, dict):
            for key, item in value.items():
                scan(key)
                scan(item)

    for value in values:
        scan(value)
    seen = set()
    while queue:
        identifier = queue.pop()
        if identifier in seen:
            continue
        seen.add(identifier)
        require(len(seen) <= 20000, 'Too many referenced library records')
        for group, directory in LIBRARY_FOLDERS.items():
            path = root/directory/(identifier+'.json')
            if path.is_file():
                record = read(path)
                pinned.setdefault(group, {})[identifier] = put_record(folder, record)
                scan(record)
    return pinned


# ---- Committing versions ----

def summary_row(record, entry, folder):
    row = dict(record=record, name=entry.get('name', record), exportable=bool(entry.get('pack')))
    if entry.get('pack'):
        try:
            pack = get_record(folder, entry['pack'])
            weapon = next(w for w in pack['weapons'] if weapon_key(w['record']) == record)
            row['configurations'] = len(weapon.get('captures', []))
            row['magazines'] = len(weapon.get('magazineObjects', []))
            row['template'] = weapon.get('template', '')
        except (ValueError, OSError, StopIteration, KeyError):
            row['exportable'] = False
    if isinstance(entry.get('status'), dict) and entry['status'].get('progress'):
        row['progress'] = entry['status']['progress']
    if entry.get('packReport', {}).get('failed'):
        row['notUpdated'] = entry['packReport']['failed']
    return row


def commit(folder, plugin, weapons, library, source=None, parent=None, origin='save', note='', moment=None, provenance=None):
    """Writes one new version containing `weapons` (record -> entry) and makes
    it the latest. The caller holds the project lock."""
    cancellation()
    project = read_project(folder) or empty_project(plugin)
    sequence = project['nextSequence']
    moment = moment or now()
    identifier = save_id(moment, sequence)
    require(not (folder/'saves'/identifier).exists(), 'A saved version with this name already exists')
    snapshot = dict(format=SNAPSHOT_FORMAT, version=1, plugin=project['plugin'], id=identifier, sequence=sequence,
                    savedAt=iso(moment), parent=parent, origin=origin, weapons=weapons, libraryRecords=library)
    if source:
        snapshot['source'] = source
    if provenance:
        snapshot['provenance'] = provenance
    if note:
        snapshot['note'] = note
    stage = folder/'saves'/('.pending-'+uuid.uuid4().hex)
    stage.mkdir(parents=True)
    try:
        write(stage/'snapshot.json', snapshot)
        read_snapshot_at(stage/'snapshot.json', identifier)
        cancellation()
        os.replace(stage, folder/'saves'/identifier)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    info = dict(id=identifier, sequence=sequence, savedAt=snapshot['savedAt'], origin=origin,
                weapons=[summary_row(r, e, folder) for r, e in sorted(weapons.items())])
    if note:
        info['note'] = note
    if provenance:
        info['provenance'] = provenance
    project['saves'].append(info)
    project['nextSequence'] = sequence+1
    # The index is replaced last: only now is this version the latest.
    write_project(folder, project)
    return info


def read_snapshot_at(path, identifier):
    snapshot = read(path, 32*1024*1024)
    require(snapshot.get('format') == SNAPSHOT_FORMAT and snapshot.get('id') == identifier, 'Saved version did not read back')
    return snapshot


def delete_save(workspace, plugin, save):
    """Deletes exactly one version. The next newest becomes latest; the export
    identity, shared records and assets stay."""
    folder = project_folder(workspace, plugin)
    with locked(folder):
        project = read_project(folder)
        require(project is not None, 'This ESP has no saved versions')
        require(any(s['id'] == save for s in project['saves']), 'Unknown saved version: '+str(save))
        project['saves'] = [s for s in project['saves'] if s['id'] != save]
        write_project(folder, project)
        target = folder/'saves'/save
        if target.is_dir():
            # Move aside first so a half-removed folder is never listed.
            trash = folder/'saves'/('.deleted-'+uuid.uuid4().hex)
            os.replace(target, trash)
            shutil.rmtree(trash, ignore_errors=True)
        return dict(ok=True, plugin=project['plugin'], deleted=save, latest=project['latest'], saves=project['saves'])


# ---- The editor's deliberate save ----

def authoring_record(document):
    """An editor weapon record without its source location, which the version
    keeps beside it as metadata."""
    require(isinstance(document, dict) and document.get('format') == 1 and
            document.get('kind') in ('reload-maker-v2-project', AUTHORING_KIND), 'Invalid weapon authoring record')
    record = {k: v for k, v in document.items() if k not in SOURCE_FIELDS}
    record['kind'] = AUTHORING_KIND
    return record, {k: document[k] for k in SOURCE_FIELDS if k in document}


def save_weapon_pack(folder, plugin, record, entry, request, identity_value, stage, backends):
    """Applies the weapon's captured configurations to its pack project. A
    configuration that cannot be saved keeps its previous behavior and is
    reported; it never blocks the other configurations or weapons."""
    from product_authoring import save as save_captures
    captures = sorted(Path(request['captures']).glob('*.json')) if request.get('captures') else []
    report = dict(saved=0, failed=0, rows=[], exceptions=request.get('captureExceptions', []))
    report['failed'] = len(report['exceptions'])
    if not captures:
        return entry.get('pack'), report
    work = stage/record.replace(':', '-')
    work.mkdir(parents=True)
    project_path = work/'pack.project.json'
    existing_id = None
    if entry.get('pack'):
        materialize_pack(folder, entry['pack'], project_path)
        existing_id = read(project_path)['id']
    namespace = pack_namespace(plugin, record, identity_value, existing_id)
    models = {}
    for magazine, pair in (request.get('models') or {}).items():
        full, empty = pair.get('full', ''), pair.get('empty', '')
        if full and empty and Path(full).is_file() and Path(empty).is_file():
            models[weapon_key(magazine)] = (full, empty)
    result = save_captures(project_path, [str(p) for p in captures], backends, namespace, models=models, strict=False)
    report['saved'] = result.get('saved', 0)
    report['failed'] += result.get('failed', 0)
    report['rows'] = [dict(selection=r.get('selection', {}), status=r['status'], error=r.get('error', '')) for r in result.get('rows', [])]
    if not result.get('saved'):
        return entry.get('pack'), report
    identity_value['packIds'][record] = namespace
    return ingest_pack(folder, project_path), report


def save_request(workspace, request_path, backends):
    """Commits the editor's working ESP project as a new latest version."""
    request = read(Path(request_path).resolve())
    require(request.get('format') == 'reload-maker-library-save-request' and request.get('version') == 1,
            'Invalid library save request')
    plugin = request['plugin']
    folder = project_folder(workspace, plugin)
    with locked(folder):
        project = read_project(folder)
        base = request.get('base') or ''
        weapons = {}
        library = {}
        if base:
            require(project is not None and any(s['id'] == base for s in project['saves']),
                    'The version these edits started from was deleted; save them again after reloading')
            snapshot = read_snapshot(folder, base)
            weapons = copy.deepcopy(snapshot['weapons'])
            library = copy.deepcopy(snapshot.get('libraryRecords', {}))
        identity_value = read_identity(folder) or new_identity(plugin)
        stage = Path(workspace).resolve()/'local/tmp/library'/uuid.uuid4().hex
        stage.mkdir(parents=True)
        reports = {}
        try:
            for record, change in sorted(request.get('weapons', {}).items()):
                cancellation()
                record = weapon_key(record)
                require(record.split(':')[0] == plugin_key(plugin), 'A weapon from another plugin cannot join this ESP project')
                authoring, source = authoring_record(read(Path(change['authoring'])))
                require(weapon_key(authoring.get('weapon', '')) == record, 'Weapon authoring belongs to another record')
                entry = weapons.get(record, {})
                entry = dict(entry, name=change.get('name') or entry.get('name') or record,
                             authoring=put_record(folder, authoring))
                if change.get('editorId'):
                    entry['editorId'] = change['editorId']
                if source:
                    entry['source'] = source
                if isinstance(change.get('status'), dict):
                    entry['status'] = change['status']
                pack, report = save_weapon_pack(folder, plugin, record, entry, change, identity_value, stage, backends)
                if pack:
                    entry['pack'] = pack
                if report['saved'] or report['failed']:
                    entry['packReport'] = report
                    reports[record] = report
                pin_library_records(folder, workspace, [authoring], library)
                weapons[record] = entry
            require(weapons, 'Nothing to save: this ESP project has no authored weapons')
            cancellation()
            info = commit(folder, plugin, weapons, library, request.get('source'), base or None, 'save', request.get('note', ''))
            if identity_value['packIds']:
                write(folder/'export/identity.json', identity_value)
        finally:
            shutil.rmtree(stage, ignore_errors=True)
    remember_bench_source(workspace, request.get('source') or {}, request.get('weapons', {}))
    return dict(ok=True, plugin=plugin, save=info, packReports=reports)


def remember_bench_source(workspace, source, weapons):
    """A weapon saved from the developer's dev bench appears in its testbed list
    with that source. The list names sources only, never authoring."""
    package = str(source.get('package') or '')
    bench = str((Path(workspace).resolve()/'local/devbench')).replace('\\', '/').lower()+'/'
    if source.get('mode') != 'package' or not package:
        return
    from product_library_migrate import bench_source
    if not bench_source(package, bench):
        return
    registry = Path(workspace).resolve()/'workspace/library/development-sources.json'
    value = read(registry) if registry.is_file() else dict(format='reload-maker-development-sources', version=1, sources=[])
    entries = {entry['id']: entry for entry in value.get('sources', [])}
    for record, change in weapons.items():
        plugin, local = record.split(':')
        slug = re.sub(r'[^a-z0-9]+', '-', Path(plugin).stem.lower()+'-'+local.lower())
        entries['bench-'+slug] = dict(id='bench-'+slug, weapon=record, source=package, bench=True,
                                      gameData=str(source.get('gameData', '')), plugin=plugin, name=change.get('name') or record)
    value['sources'] = sorted(entries.values(), key=lambda e: (e.get('bench', False), e['id']))
    write(registry, value)


def project_state(workspace, plugin):
    folder = project_folder(workspace, plugin)
    project = read_project(folder)
    if project is None:
        return dict(ok=True, plugin=plugin, key=plugin_key(plugin), latest=None, saves=[])
    return dict(ok=True, plugin=project['plugin'], key=project['key'], latest=project['latest'], saves=project['saves'],
                published=read_published(folder)['publications'][-8:])
