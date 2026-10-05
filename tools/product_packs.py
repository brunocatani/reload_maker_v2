"""Finished-pack validation and stable local object-plugin generation.

Native plugin and NIF operations are delegated to the bundled approved adapters.
An atomic current.json selects a complete generation, including its ID ledger.
"""
import asyncio
import copy
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import struct
import shutil
import uuid
import yaml

PLUGIN = 'DIAMOND_Generated.esp'
TEMPLATES = {
    'pistol.slide-detachable.v1',
    'rifle.ar15-detachable.v1',
    'rifle.operating-handle-rocking.v1',
    'rifle.reciprocating-detachable.v1',
    'sniper.manual-bolt-detachable.v1',
}


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def cancellation():
    marker = os.environ.get('RELOAD_MAKER_CANCEL_FILE')
    if marker and Path(marker).exists():
        raise RuntimeError('Build cancelled; the last good generation was retained')


def digest(file):
    with file.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read(file, limit=64*1024*1024):
    require(file.is_file() and file.stat().st_size <= limit, f'Missing or oversized file: {file}')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f'Duplicate JSON key: {key}')
            result[key] = value
        return result
    return json.loads(file.read_text(encoding='utf-8'), object_pairs_hook=unique,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError(f'Nonfinite JSON: {value}')))


def write(file, value):
    file.parent.mkdir(parents=True, exist_ok=True)
    temporary = file.with_name(file.name+'.pending')
    temporary.write_text(json.dumps(value, sort_keys=True, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    os.replace(temporary, file)


def relative(value):
    require(isinstance(value, str) and 0 < len(value) < 512, 'Invalid relative content path')
    value = value.replace('\\', '/')
    path = PurePosixPath(value)
    require(not path.is_absolute() and all(p not in ('', '.', '..') and ':' not in p for p in value.split('/')), f'Unsafe content path: {value}')
    return value


def within(root, value):
    file = (root/relative(value)).resolve()
    require(file.is_relative_to(root.resolve()), f'Content escapes pack: {value}')
    return file


def identity(value):
    require(isinstance(value, str) and re.fullmatch(r'[a-z0-9][a-z0-9._/-]{0,199}', value) is not None and '..' not in value, 'Invalid logical identity')
    return value


def load_pack(root):
    root = root.resolve()
    manifest_path=root/'manifest.json'
    if not manifest_path.is_file():
        manifests=list(root.glob('F4SE/Plugins/DIAMOND/Packs/*/manifest.json'))
        require(len(manifests)==1, 'Select a folder containing one complete reload pack')
        manifest_path=manifests[0]
    manifest = read(manifest_path, 1024*1024)
    require(manifest.get('format') == 'diamond-content-pack' and manifest.get('version') == 1, 'Unsupported content pack')
    pack = identity(manifest.get('id'))
    require('/' not in pack, 'Pack ID must be a single namespace')
    require(isinstance(manifest.get('revision'), int) and manifest['revision'] > 0, 'Missing pack revision')
    required = manifest.get('templates')
    require(isinstance(required, list) and required and all(t in TEMPLATES for t in required), f'Unsupported template in {pack}')
    weapons = manifest.get('weapons')
    require(isinstance(weapons, list) and weapons and all(isinstance(w,dict) for w in weapons),
            'A finished reload pack needs weapon behavior, hand fits, sounds and grab policies; object-only packs are incomplete')
    files = manifest.get('files')
    require(isinstance(files, list) and 0 < len(files) <= 10000, 'Export this pack again: its file list is missing or from an older Maker')
    normalized = set()
    for path in files:
        key = relative(path).casefold()
        require(key not in normalized, 'Case-ambiguous pack file')
        require(key.startswith(f'f4se/plugins/diamond/packs/{pack}/') or key.startswith(f'meshes/diamond/packs/{pack}/'),
                f'Pack file is outside its namespace: {path}')
        require(within(root, path).is_file(), f'Pack file is missing: {path}')
        normalized.add(key)
    objects_path = relative(manifest.get('objects'))
    require(objects_path in files, 'Object definitions are not frozen in the pack')
    objects = read(within(root, objects_path), 8*1024*1024)
    require(isinstance(objects, list) and 0 < len(objects) <= 2048, 'Invalid object count')
    ids = set()
    for obj in objects:
        key = identity(obj.get('id'))
        require(key.startswith(pack+'/') and key not in ids, 'Duplicate or foreign object identity')
        ids.add(key)
        model = relative(obj.get('model'))
        require(model.casefold().startswith('meshes/diamond/packs/'+pack+'/') and model.casefold().endswith('.nif'), 'Object model is outside its pack namespace')
        require(model in files, 'Object model is not a frozen finished asset')
        bounds = obj.get('bounds')
        require(isinstance(bounds, list) and len(bounds) == 6 and all(type(v) is int and -32768 <= v <= 32767 for v in bounds), 'Invalid object bounds')
        require(all(bounds[i] < bounds[i+3] for i in range(3)), 'Empty object bounds')
        require(isinstance(obj.get('name'), str) and 0 < len(obj['name']) <= 256, 'Invalid object name')
        require(type(obj.get('weight')) in (int, float) and math.isfinite(obj['weight']) and 0 <= obj['weight'] < 10000, 'Invalid inventory weight')
        require(type(obj.get('value')) is int and 0 <= obj['value'] <= 0x7fffffff, 'Invalid inventory value')
    return dict(root=root, manifestPath=manifest_path, manifest=manifest, objects=objects)


def allocate(previous, packs):
    ledger = copy.deepcopy(previous)
    require(ledger.get('version') == 1 and ledger.get('plugin') == PLUGIN, 'Invalid installation ledger')
    entries = ledger.get('objects')
    require(isinstance(entries, dict) and len(entries) <= 8192, 'Invalid installation object count')
    used = set()
    for key, entry in entries.items():
        identity(key)
        value = entry.get('localId')
        require(type(value) is int and 0x800 <= value <= 0xffffff and value not in used, 'Corrupt or duplicate object allocation')
        used.add(value)
        entries[key] = dict(localId=value, active=False)
    next_id = ledger.get('nextId')
    require(type(next_id) is int and next_id > max(used, default=0x7ff) and next_id <= 0x1000000, 'Invalid ledger high-water mark')
    active = set()
    for pack in sorted(packs, key=lambda p: p['manifest']['id']):
        for obj in sorted(pack['objects'], key=lambda o: o['id']):
            key = obj['id']
            require(key not in active, 'Two active packs define the same object')
            active.add(key)
            if key not in entries:
                require(next_id <= 0xffffff and len(entries) < 8192, 'Generated object allocation is full')
                entries[key] = dict(localId=next_id)
                next_id += 1
            entries[key].update(definition=obj, active=True, pack=pack['manifest']['id'])
    ledger['nextId'] = next_id
    return ledger


def write_plugin_source(folder, ledger):
    folder.mkdir(parents=True)
    header = dict(SpriggitSource=dict(PackageName='Spriggit.Yaml.Fallout4', Version='0.40'),
                  ModKey=PLUGIN, GameRelease='Fallout4VR',
                  ModHeader=dict(Stats=dict(Version=.95, NextFormID=ledger['nextId']),
                                 Author='Reload Maker', Description='Generated physical reload objects.',
                                 MasterReferences=[dict(Master='Fallout4.esm', FileSize=0)], INTV='0x01000000'))
    (folder/'RecordData.yaml').write_text(yaml.safe_dump(header, sort_keys=False), encoding='utf-8')
    write(folder/'spriggit-meta.json', dict(PackageName='Spriggit.Yaml.Fallout4', Version='0.40', Release='Fallout4VR', ModKey=PLUGIN))
    records = folder/'MiscItems'
    records.mkdir()
    for key, entry in ledger['objects'].items():
        if not entry['active']:
            continue
        obj = entry['definition']
        editor = 'DiamondObject_'+hashlib.sha256(key.encode()).hexdigest()[:24]
        entry['editorId'] = editor
        record = dict(FormKey=f'{entry["localId"]:06X}:{PLUGIN}', EditorID=editor,
                      ObjectBounds=dict(First=', '.join(map(str, obj['bounds'][:3])), Second=', '.join(map(str, obj['bounds'][3:]))),
                      Name=dict(TargetLanguage='English', Value=obj['name']),
                      Model=dict(File=relative(obj['model'])[7:].replace('/', '\\'), Data='0x0400000000000000000000000000000000000000'),
                      Value=obj['value'], Weight=obj['weight'])
        (records/(editor+'.yaml')).write_text(yaml.safe_dump(record, sort_keys=False), encoding='utf-8')


async def verify_plugin(backend, plugin, ledger):
    active = {key: value for key, value in ledger['objects'].items() if value['active']}
    header = json.loads(await backend.fo4_plugin_info(backend.PluginPathInput(plugin_path=str(plugin))))
    masters = [value if value.lower().endswith('.esm') else bytes.fromhex(value).rstrip(b'\0').decode('utf-8') for value in header.get('masters', [])]
    require(masters == ['Fallout4.esm'], 'Generated plugin has unexpected hard masters')
    result = json.loads(await backend.fo4_read_records(backend.ReadRecordsInput(plugin_path=str(plugin), record_type='MISC', limit=10000)))
    require('error' not in result and result['returned'] == len(active), f'Generated record count/readback failed: {result.get("error")}')
    require(all(int(r['form_id'], 16) >> 24 == len(masters) for r in result['records']), 'Generated record belongs to a master instead of this plugin')
    actual = {int(r['form_id'], 16) & 0xffffff: {s['type']: s['value'] for s in r['subrecords']} for r in result['records']}
    require(len(actual) == len(active), 'Duplicate generated record IDs')
    for entry in active.values():
        row = actual.get(entry['localId'], {})
        require(row.get('EDID') == entry['editorId'], 'Generated object identity mismatch')
        # The approved plugin reader returns MODL as a raw field; decode that
        # field, without another plugin parser in the builder.
        model = bytes.fromhex(row.get('MODL', '')).rstrip(b'\0').decode('utf-8').replace('\\', '/')
        require(model.casefold() == relative(entry['definition']['model'])[7:].casefold(), 'Generated model mapping mismatch')
        obj = entry['definition']
        require(list(struct.unpack('<6h', bytes.fromhex(row.get('OBND', '')))) == obj['bounds'], 'Generated bounds mismatch')
        value, weight = struct.unpack('<If', bytes.fromhex(row.get('DATA', '')))
        require(value == obj['value'] and math.isclose(weight, obj['weight'], rel_tol=1e-6, abs_tol=1e-6), 'Generated inventory values mismatch')
        require(row.get('FULL') == obj['name'], 'Generated object name mismatch')


async def build(packs, installation, backends, initialize=False):
    installation = installation.resolve()
    installation.mkdir(parents=True, exist_ok=True)
    lock = installation/'build.lock'
    with lock.open('x', encoding='utf-8') as stream:
        stream.write(str(os.getpid()))
    try:
        cancellation()
        current = installation/'current.json'
        old = None
        if current.exists():
            pointer = read(current, 16384)
            old = within(installation, pointer['generation'])
            ledger = read(old/'ledger.json')
        else:
            completed = any((folder/'receipt.json').exists() for folder in (installation/'generations').glob('*'))
            require(initialize and not completed, 'Use explicit initialization for a new installation; restore current.json for an existing one')
            seed = installation/'identity.json'
            if not seed.exists():
                write(seed, dict(version=1, plugin=PLUGIN, installation=str(uuid.uuid4()), nextId=0x800, objects={}))
            ledger = read(seed)
            require(ledger.get('nextId') == 0x800 and ledger.get('objects') == {}, 'Invalid new installation identity')
        loaded = [load_pack(Path(p)) for p in packs]
        limits = backends.content_validation()['limits']
        require(loaded and len(loaded) <= limits['packs'], f"Select between one and {limits['packs']} finished packs")
        require(len({p['manifest']['id'] for p in loaded}) == len(loaded), 'Duplicate pack ID')
        require(sum(len(p['objects']) for p in loaded) <= limits['worldObjects'],
                f"Selected packs exceed DIAMOND's capacity of {limits['worldObjects']} full/empty world objects; reduce the selection")
        ledger = allocate(ledger, loaded)
        generation = 'generations/'+uuid.uuid4().hex
        stage = installation/generation
        output = stage/'output'
        output.mkdir(parents=True)
        seen = set()
        native = backends.adapter('nif')
        for pack in loaded:
            for obj in pack['objects']:
                cancellation()
                source = within(pack['root'], obj['model'])
                key = obj['model'].casefold()
                if key not in seen:
                    validation = native.nif_validate(str(source))
                    require(validation.get('ok'), f'Invalid finished NIF: {source}')
                    info = native.nif_info(str(source))
                    data = info.get('data', {})
                    types = data.get('blockTypes', {})
                    require(info.get('ok') and data.get('bsVersion') == 130 and
                            types.get('bhkNPCollisionObject') == 1 and types.get('bhkPhysicsSystem') == 1,
                            f'Model has no cooked Fallout 4 collision: {source}')
                    # Authored packs own their finished assets. Copying models
                    # into the generated companion would mask later pack updates
                    # in MO2.
                    seen.add(key)
        write_plugin_source(stage/'plugin-source', ledger)
        backends.prepare_serializer()
        backend = backends.adapter('plugin')
        result = json.loads(await backend.fo4_serialize_plugin(backend.SpriggitSerializeInput(yaml_dir=str(stage/'plugin-source'), output_path=str(output/PLUGIN))))
        if result.get('status') != 'success':
            log = stage/'serializer.log'
            log.write_text(result.get('error', 'No native tool diagnostics'), encoding='utf-8')
            raise RuntimeError(f'The plugin serializer failed. The last good output is unchanged. Diagnostics: {log}')
        cancellation()
        await verify_plugin(backend, output/PLUGIN, ledger)
        bindings = dict(format='diamond-object-bindings', version=1, installation=ledger['installation'],
                        generation=generation.split('/')[-1], plugin=PLUGIN,
                        packs=sorted(p['manifest']['id'] for p in loaded),
                        objects={key: dict(form=f'{PLUGIN}:{entry["localId"]:06x}', editorId=entry['editorId'],
                                           model=entry['definition']['model'][7:], name=entry['definition']['name'],
                                           active=entry['active'])
                                 for key, entry in ledger['objects'].items() if entry['active']})
        write(output/'F4SE/Plugins/DIAMOND/Generated/objects.json', bindings)
        # Validate the exact effective Data view with the runtime catalog loader.
        # Validate independent copies of the effective content. The companion
        # output remains free of authored assets so it cannot mask pack updates.
        view = stage/'validation-Data'
        def snapshot_copy(source, destination):
            destination.parent.mkdir(parents=True, exist_ok=True)
            # Pack files live in their own pack's namespace.
            require(not destination.exists(), 'Two selected packs ship '+str(destination))
            shutil.copy2(source, destination)
        for pack in loaded:
            cancellation()
            for name in pack['manifest']['files']:
                snapshot_copy(within(pack['root'], name), within(view, name))
            snapshot_copy(pack['manifestPath'], view/'F4SE/Plugins/DIAMOND/Packs'/pack['manifest']['id']/'manifest.json')
        snapshot_copy(output/PLUGIN, view/PLUGIN)
        snapshot_copy(output/'F4SE/Plugins/DIAMOND/Generated/objects.json', view/'F4SE/Plugins/DIAMOND/Generated/objects.json')
        checked = backends.content_validation(view)
        checked['expectedObjects'] = sum(len(p['objects']) for p in loaded)
        write(stage/'catalog-validation.json', checked)
        require(checked.get('ok') and checked.get('objects') == sum(len(p['objects']) for p in loaded),
                'DIAMOND rejected the complete generated catalog: '+json.dumps(checked)+
                f'. Review {stage / "catalog-validation.json"}')
        write(stage/'ledger.json', ledger)
        receipt = dict(version=1, generation=generation)
        cancellation()
        # Only mark a generation complete after cancellation is checked.
        # Otherwise a cancelled first build would forbid initialize retry even
        # though there has never been a current.json to restore.
        write(stage/'receipt.json', receipt)
        write(current, receipt)
        return dict(ok=True, output=str(output), generation=receipt, activeObjects=sum(e['active'] for e in ledger['objects'].values()),
                    runtimeCatalog=checked)
    finally:
        lock.unlink()
