"""Complete, independently selectable source-plugin ESL distributions."""
import copy
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid
import zipfile
import yaml

from product_packs import (allocate, cancellation, identity, load_pack, read,
                           relative, require, verify_plugin, within, write,
                           write_plugin_source)


def plugin_name(master):
    require(isinstance(master, str) and re.fullmatch(r'[^:/\\\x00]+\.(esm|esp|esl)', master, re.I),
            'Invalid source plugin name')
    stem = re.sub(r'[^a-z0-9_-]+', '_', Path(master).stem.lower()).strip('_')
    require(stem and len(stem) <= 100, 'Source plugin name cannot form an ESL filename')
    return 'DIAMOND_' + stem + '.esl'


def load_inputs(inputs, stage, backends):
    from product_export import export_pack
    packs = []
    for index, value in enumerate(inputs):
        source = Path(value).resolve()
        if source.is_file():
            frozen = stage/'inputs'/str(index)
            export_pack(str(source), str(frozen), backends)
            source = frozen
        manifests = sorted(source.glob('F4SE/Plugins/DIAMOND/Packs/*/manifest.json'))
        if manifests:
            packs.extend(load_pack(source, path) for path in manifests)
        else:
            packs.append(load_pack(source))
    require(packs and len({p['manifest']['id'] for p in packs}) == len(packs), 'Duplicate or empty pack selection')
    return packs


def retain_group_members(packs, published, installation, ledger):
    if published is None:
        return packs
    masters = {w['record'].rsplit(':',1)[0].lower() for p in packs for w in p['manifest']['weapons']}
    projects = {p['manifest'].get('authoringId',p['manifest']['id']) for p in packs}
    records = {w['record'].lower() for p in packs for w in p['manifest']['weapons']}
    roots = set()
    for master in masters:
        generation = ledger.get('groups', {}).get(master, {}).get('generation')
        saved = within(installation, generation) if generation else published
        root = saved/'output'
        if not list(root.glob('F4SE/Plugins/DIAMOND/Packs/*/manifest.json')):
            root = saved/'validation-Data'
        roots.add(root)
    result = list(packs)
    for manifest in sorted(path for root in roots for path in root.glob('F4SE/Plugins/DIAMOND/Packs/*/manifest.json')):
        root = manifest.parents[5]
        metadata = read(manifest)
        if metadata.get('authoringId',metadata['id']) in projects:
            continue
        selected = [w for w in metadata['weapons'] if w['record'].rsplit(':',1)[0].lower() in masters and w['record'].lower() not in records]
        if not selected:
            continue
        pack = load_pack(root,manifest)
        needed = {a[k] for w in selected for a in w['magazineObjects'] for k in ('full','empty')}
        pack['manifest']['weapons'] = selected
        pack['objects'] = [o for o in pack['objects'] if o['id'] in needed]
        result.append(pack)
    return result


def partition(packs):
    groups, names, claims = {}, {}, set()
    for pack in packs:
        by_master = {}
        for weapon in pack['manifest']['weapons']:
            record = weapon['record'].lower()
            require(record not in claims, 'Two selected packs supply the same weapon: '+record)
            claims.add(record)
            master = record.rsplit(':', 1)[0]
            by_master.setdefault(master, []).append(weapon)
        for master, weapons in by_master.items():
            plugin = plugin_name(master)
            require(names.get(plugin, master) == master, 'Source filenames collide after normalization: '+master)
            names[plugin] = master
            identifiers = {a[state] for w in weapons for a in w['magazineObjects'] for state in ('full', 'empty')}
            objects = [copy.deepcopy(o) for o in pack['objects'] if o['id'] in identifiers]
            require({o['id'] for o in objects} == identifiers, 'Incomplete magazine objects in '+master)
            original = pack['manifest']['id']
            authoring_id = pack['manifest'].get('authoringId', original)
            output_id = authoring_id+'.'+Path(plugin).stem.lower()
            remap = {o['id']: output_id+o['id'][len(original):] for o in objects}
            for obj in objects: obj['id'] = remap[obj['id']]
            weapons = copy.deepcopy(weapons)
            for weapon in weapons:
                for assignment in weapon['magazineObjects']:
                    for state in ('full','empty'): assignment[state] = remap[assignment[state]]
            identity(output_id)
            require(len(output_id) <= 128, 'Split pack namespace is too long')
            groups.setdefault(master, []).append(dict(source=pack, weapons=weapons, objects=objects,
                                                       id=output_id))
    return groups


def copy_file(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        require(source.read_bytes() == target.read_bytes(), 'Conflicting bundled file: '+str(target))
    else:
        shutil.copy2(source, target)


def emit_pack(part, plugin, output, native, validated):
    pack = part['source']; original = pack['manifest']['id']; output_id = part['id']
    prefix = 'F4SE/Plugins/DIAMOND/Packs/'+output_id+'/'
    old_prefix = 'F4SE/Plugins/DIAMOND/Packs/'+original+'/'
    manifest = copy.deepcopy(pack['manifest'])
    manifest.update(id=output_id, authoringId=manifest.get('authoringId',original), generatedPlugin=plugin,
                    objectBindings='F4SE/Plugins/DIAMOND/Generated/'+Path(plugin).stem+'/objects.json')
    files, weapons = set(), []
    for obj in part['objects']:
        source = within(pack['root'], obj['model'])
        cache_key = str(source)
        if cache_key not in validated:
            require(native.nif_validate(str(source)).get('ok'), 'Invalid magazine model: '+str(source))
            info = native.nif_info(str(source)); data = info.get('data', {}); kinds = data.get('blockTypes', {})
            require(info.get('ok') and data.get('bsVersion') == 130 and
                    kinds.get('bhkNPCollisionObject') == 1 and kinds.get('bhkPhysicsSystem') == 1,
                    'Magazine needs cooked Fallout 4 collision: '+str(source))
            validated.add(cache_key)
        model = 'Meshes/'+Path(plugin).stem+'/'+manifest['authoringId']+'/'+source.name
        copy_file(source, within(output, model))
        obj['model'] = model; files.add(model)
    for weapon in part['weapons']:
        item = copy.deepcopy(weapon)
        for kind in ('profiles', 'magazines', 'sounds', 'grabs'):
            if kind not in item:
                continue
            path = relative(item[kind]); require(path.startswith(old_prefix), 'Foreign behavior document')
            target = prefix+path[len(old_prefix):]
            copy_file(within(pack['root'], path), within(output, target))
            item[kind] = target; files.add(target)
        weapons.append(item)
    # Native weapon records refer to these exact paths. They are not magazine
    # object meshes and cannot be relocated without rewriting the source mod.
    for path in manifest.get('modelOverrides', []):
        copy_file(within(pack['root'], path), within(output, path)); files.add(path)
    object_path = prefix+'objects.json'; write(within(output, object_path), part['objects']); files.add(object_path)
    manifest.update(objects=object_path, files=sorted(files), weapons=weapons,
                    templates=sorted({w['template'] for w in weapons}))
    write(output/prefix/'manifest.json', manifest)
    return dict(manifest=manifest, objects=part['objects'])


def deliver(output, archive=None, mo2=None):
    require(not (archive and mo2), 'Choose Export to MO2 or Export ZIP / 7z')
    if archive:
        destination = Path(archive).resolve()
        require(destination.suffix.lower() in ('.zip', '.7z'), 'Choose a .zip or .7z archive')
        require(not destination.is_relative_to(output), 'Archive must be outside its source folder')
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_name(destination.stem+'.'+uuid.uuid4().hex+'.pending'+destination.suffix)
        try:
            if destination.suffix.lower() == '.zip':
                with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as packed:
                    for path in sorted(output.rglob('*')):
                        if path.is_file():
                            cancellation(); packed.write(path, path.relative_to(output).as_posix())
            else:
                executable = shutil.which('7z') or str(Path(os.environ.get('ProgramFiles', 'C:/Program Files'))/'7-Zip/7z.exe')
                require(Path(executable).is_file(), '7-Zip is not installed; choose ZIP instead')
                result = subprocess.run([executable, 'a', '-t7z', '-mx=5', '-mmt=2', str(temporary), '.'],
                                        cwd=output, capture_output=True, text=True, timeout=600,
                                        creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
                require(result.returncode == 0, '7-Zip export failed: '+result.stderr[-2000:])
            cancellation(); os.replace(temporary, destination)
        finally:
            if temporary.exists(): temporary.unlink()
        return dict(archive=str(destination))
    if mo2:
        destination = Path(mo2).resolve()
        require(destination != output and not destination.is_relative_to(output) and not output.is_relative_to(destination),
                'MO2 mod destination overlaps the build workspace')
        require(destination.parent.is_dir() and destination.name.lower() not in ('', '.', '..', 'data', 'mods', 'profiles')
                and not any((destination/name).exists() for name in ('ModOrganizer.ini','Fallout4.exe','Fallout4VR.exe')), 'Choose one named mod folder, not the game or MO2 instance root')
        # The receipt lists only generated files belonging to each source group.
        # Updating one group never removes another source mod's installed group.
        receipts = {read(p)['plugin']: read(p) for p in destination.glob('F4SE/Plugins/DIAMOND/Generated/*/files.json')}
        updates = [read(p) for p in output.glob('F4SE/Plugins/DIAMOND/Generated/*/files.json')]
        retained_files = {name for plugin, data in receipts.items() if plugin not in {u['plugin'] for u in updates} for name in data['files']}
        retained_files.update(name for data in updates for name in data['files'])
        for receipt in sorted(output.glob('F4SE/Plugins/DIAMOND/Generated/*/files.json')):
            current = read(receipt); old_path = destination/receipt.relative_to(output)
            old = read(old_path) if old_path.is_file() else {'files': []}
            require(old.get('plugin', current['plugin']) == current['plugin'], 'MO2 group receipt mismatch')
            for name in set(old['files'])-set(current['files']):
                if name in retained_files: continue
                path = within(destination, name)
                allowed = name.startswith('Meshes/'+Path(current['plugin']).stem+'/') or name.startswith('F4SE/Plugins/DIAMOND/Packs/') or name in old.get('modelOverrides', [])
                require(allowed, 'Unexpected stale file outside this group')
                if path.is_file(): path.unlink()
        for path in sorted(output.rglob('*')):
            if path.is_file():
                target = within(destination, path.relative_to(output).as_posix())
                target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(path, target)
        return dict(installedTo=str(destination))
    return {}


async def build_grouped(inputs, installation, backends, initialize=False, archive=None, mo2=None):
    installation = installation.resolve(); installation.mkdir(parents=True, exist_ok=True)
    lock = installation/'build.lock'
    with lock.open('x', encoding='utf-8') as stream: stream.write(str(os.getpid()))
    try:
        cancellation(); current = installation/'current.json'
        if current.exists():
            published = within(installation, read(current)['generation'])
            previous = read(published/'ledger.json')
        else:
            published = None
            require(initialize, 'Initialize this new installation ledger first')
            previous = dict(version=2, installation=str(uuid.uuid4()), groups={})
        require(previous.get('version') in (1, 2), 'Unsupported installation ledger')
        # New ESL identities are separate from the retired monolithic plugin.
        # Do not alias old world references to unrelated compacted records.
        ledger = copy.deepcopy(previous) if previous['version'] == 2 else dict(version=2, installation=previous['installation'], groups={})
        generation = 'generations/'+uuid.uuid4().hex; stage = installation/generation; output = stage/'output'
        output.mkdir(parents=True)
        loaded = retain_group_members(load_inputs(inputs, stage, backends), published, installation, previous)
        capability = backends.content_validation()
        require(capability.get('sourcePluginESL') is True, 'Update DIAMOND and its bundled validator for source-plugin ESL exports')
        limits = capability['limits']
        require(len(loaded) <= limits['packs'], 'Too many selected reload packs')
        groups = partition(loaded); native = backends.adapter('nif'); validated = set(); emitted_count = 0
        backends.prepare_serializer(); backend = backends.adapter('plugin'); plugins = []
        for master, parts in sorted(groups.items()):
            cancellation(); plugin = plugin_name(master); stem = Path(plugin).stem
            require(all(other == master or g['plugin'].lower() != plugin.lower() for other, g in ledger['groups'].items()), 'ESL filename collides with an existing source group')
            masters = ['Fallout4.esm'] + ([] if master == 'fallout4.esm' else [master])
            emitted = [emit_pack(p, plugin, output, native, validated) for p in parts]
            seed = ledger['groups'].get(master, dict(version=1, plugin=plugin, installation=ledger['installation'], nextId=0x800, objects={}))
            seed['masters'] = masters
            group = allocate(seed, emitted); require(group['nextId'] <= 0x1000, 'ESL record capacity exceeded for '+master)
            group['generation'] = generation
            ledger['groups'][master] = group
            write_plugin_source(stage/'plugin-source'/stem, group)
            result = json.loads(await backend.fo4_serialize_plugin(backend.SpriggitSerializeInput(
                yaml_dir=str(stage/'plugin-source'/stem), output_path=str(output/plugin))))
            require(result.get('status') == 'success', 'ESL serialization failed: '+str(result))
            await verify_plugin(backend, output/plugin, group)
            # Use the approved native reader to prove the emitted light flags.
            result = json.loads(await backend.fo4_deserialize_plugin(backend.SpriggitDeserializeInput(
                plugin_path=str(output/plugin), output_dir=str(stage/'readback'/stem))))
            require(result.get('status') == 'success', 'Cannot read back generated ESL flags')
            header = yaml.safe_load((stage/'readback'/stem/'RecordData.yaml').read_text(encoding='utf-8-sig'))
            require({'Master', 'Small'}.issubset(set(header['ModHeader']['Flags'])), 'Generated plugin is not a light master')
            bindings_path = 'F4SE/Plugins/DIAMOND/Generated/'+stem+'/objects.json'
            bindings = dict(format='diamond-object-bindings', version=2, plugin=plugin, sourcePlugin=master,
                            installation=ledger['installation'], packs=sorted(p['id'] for p in parts),
                            objects={key: dict(form=f'{plugin}:{entry["localId"]:06x}', editorId=entry['editorId'],
                                               model=entry['definition']['model'][7:], name=entry['definition']['name'], active=True)
                                     for key, entry in group['objects'].items() if entry['active']})
            write(output/bindings_path, bindings)
            files = {plugin, bindings_path}
            for pack in emitted:
                files.update(pack['manifest']['files'])
                files.add('F4SE/Plugins/DIAMOND/Packs/'+pack['manifest']['id']+'/manifest.json')
            write(output/'F4SE/Plugins/DIAMOND/Generated'/stem/'files.json', dict(plugin=plugin, sourcePlugin=master, files=sorted(files), modelOverrides=sorted({path for p in emitted for path in p['manifest'].get('modelOverrides', [])})))
            plugins.append(dict(plugin=plugin, master=master, objects=len(bindings['objects'])))
            emitted_count += len(bindings['objects'])
        require(emitted_count <= limits['worldObjects'], 'Selected active objects exceed DIAMOND runtime capacity')
        (output/'DIAMOND Reload Packs.txt').write_text('Install this bundle as one MO2 mod. Enable only the ESLs for the original source mods you use.\nRequires DIAMOND with source-plugin ESL support. Fallout 4 VR also requires an ESL loader such as Daytripper.\nGenerated magazine meshes are grouped under each ESL name. Weapon model overrides retain their required native paths.\nSource plugins, activation and load order are not modified by the exporter.\n', encoding='utf-8')
        checked = backends.content_validation(output)
        write(stage/'catalog-validation.json', checked)
        require(checked.get('ok') and checked.get('objects') == emitted_count, 'DIAMOND rejected grouped content: '+json.dumps(checked))
        write(stage/'ledger.json', ledger); cancellation()
        delivered = deliver(output, archive, mo2)
        receipt = dict(version=2, generation=generation)
        write(stage/'receipt.json', receipt); write(current, receipt)
        return dict(ok=True, output=str(output), generation=receipt, plugins=plugins,
                    activeObjects=emitted_count, runtimeCatalog=checked, **delivered)
    finally:
        lock.unlink()
