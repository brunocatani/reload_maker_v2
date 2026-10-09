"""Complete, independently selectable source-plugin ESL distributions."""
import contextlib
import copy
import filecmp
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import uuid
import zipfile
import yaml

from product_packs import (allocate, cancellation, identity, load_pack, read,
                           relative, require, within, write,
                           write_plugin_source)



# Increment when the emitted format changes. These are disposable prepared
# outputs, keyed by readable weapon/source identities, never admission gates.
EXPORT_CACHE_VERSION = 1


def restore_export(cache, inputs, destination):
    state = cache/'state.json'
    if not state.is_file():
        return None
    try:
        value = read(state)
        if value.get('version') != EXPORT_CACHE_VERSION or value.get('inputs') != inputs:
            return None
        for name, stamp in value['files'].items():
            path = within(cache/'output', name)
            if not path.is_file() or [path.stat().st_size, path.stat().st_mtime_ns] != stamp:
                return None
        for name in value['files']:
            copy_file(within(cache/'output', name), within(destination, name))
        return value['result']
    except (OSError, ValueError, KeyError):
        return None


def remember_export(cache, inputs, output, result):
    # Publish the marker last: an interrupted refresh is simply rebuilt.
    cache.mkdir(parents=True, exist_ok=True)
    (cache/'state.json').unlink(missing_ok=True)
    shutil.rmtree(cache/'output', ignore_errors=True)
    shutil.copytree(output, cache/'output')
    files = {p.relative_to(cache/'output').as_posix(): [p.stat().st_size, p.stat().st_mtime_ns]
             for p in (cache/'output').rglob('*') if p.is_file()}
    write(cache/'state.json', dict(version=EXPORT_CACHE_VERSION, inputs=inputs, files=files, result=result))


def plugin_name(master):
    require(isinstance(master, str) and re.fullmatch(r'[^:/\\\x00]+\.(esm|esp|esl)', master, re.I),
            'Invalid source plugin name')
    stem = re.sub(r'[^a-z0-9_-]+', '_', Path(master).stem.lower()).strip('_')
    require(stem and len(stem) <= 100, 'Source plugin name cannot form a light ESP filename')
    return 'DIAMOND_' + stem + '.esp'


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
        receipts = {Path(read(p)['plugin']).stem.lower(): read(p) for p in destination.glob('F4SE/Plugins/DIAMOND/Generated/*/files.json')}
        updates = [read(p) for p in output.glob('F4SE/Plugins/DIAMOND/Generated/*/files.json')]
        retained_files = {name for plugin, data in receipts.items() if plugin not in {Path(u['plugin']).stem.lower() for u in updates} for name in data['files']}
        retained_files.update(name for data in updates for name in data['files'])
        for receipt in sorted(output.glob('F4SE/Plugins/DIAMOND/Generated/*/files.json')):
            current = read(receipt); old_path = destination/receipt.relative_to(output)
            old = read(old_path) if old_path.is_file() else {'files': []}
            previous_plugin = old.get('plugin', current['plugin'])
            renamed = previous_plugin == str(Path(current['plugin']).with_suffix('.esl'))
            require(previous_plugin == current['plugin'] or renamed, 'MO2 group receipt mismatch')
            for name in set(old['files'])-set(current['files']):
                if name in retained_files: continue
                path = within(destination, name)
                allowed = name.startswith('Meshes/'+Path(current['plugin']).stem+'/') or name.startswith('F4SE/Plugins/DIAMOND/Packs/') or name in old.get('modelOverrides', []) or (renamed and name == previous_plugin)
                require(allowed, 'Unexpected stale file outside this group')
                if path.is_file(): path.unlink()
        # Identical files are left alone; changed and new files are replaced
        # whole. Comparing bytes only avoids copying, it admits nothing.
        written = unchanged = 0
        for path in sorted(output.rglob('*')):
            if not path.is_file():
                continue
            cancellation()
            target = within(destination, path.relative_to(output).as_posix())
            if target.is_file() and target.stat().st_size == path.stat().st_size and filecmp.cmp(path, target, shallow=False):
                unchanged += 1
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            pending = target.with_name(target.name+'.'+uuid.uuid4().hex+'.pending')
            try:
                shutil.copy2(path, pending)
                os.replace(pending, target)
            finally:
                if pending.exists():
                    pending.unlink()
            written += 1
        return dict(installedTo=str(destination), written=written, unchanged=unchanged)
    return {}


async def emit_group(master, parts, seed, installation, output, stage, backend, native, validated):
    """Builds one source plugin's ESL-flagged ESP, bindings and file receipt.
    `seed` is the group's previous allocation ledger, or None for a new group."""
    plugin = plugin_name(master); stem = Path(plugin).stem
    masters = ['Fallout4.esm'] + ([] if master == 'fallout4.esm' else [master])
    emitted = [emit_pack(p, plugin, output, native, validated) for p in parts]
    seed = copy.deepcopy(seed) if seed else dict(version=1, plugin=plugin, installation=installation, nextId=0x800, objects={})
    require(seed['plugin'] in (plugin, str(Path(plugin).with_suffix('.esl'))), 'Unexpected source group plugin identity')
    seed['plugin'] = plugin
    seed['masters'] = masters
    seed.pop('generation', None)
    group = allocate(seed, emitted); require(group['nextId'] <= 0x1000, 'ESL record capacity exceeded for '+master)
    write_plugin_source(stage/'plugin-source'/stem, group)
    result = json.loads(await backend.fo4_serialize_plugin(backend.SpriggitSerializeInput(
        yaml_dir=str(stage/'plugin-source'/stem), output_path=str(output/plugin))))
    require(result.get('status') == 'success', 'ESL serialization failed: '+str(result))
    # Use the approved native reader to prove the emitted light flags.
    result = json.loads(await backend.fo4_deserialize_plugin(backend.SpriggitDeserializeInput(
        plugin_path=str(output/plugin), output_dir=str(stage/'readback'/stem))))
    require(result.get('status') == 'success', 'Cannot read back generated ESL flags')
    header = yaml.safe_load((stage/'readback'/stem/'RecordData.yaml').read_text(encoding='utf-8-sig'))
    flags = set(header['ModHeader']['Flags'])
    require('Small' in flags and 'Master' not in flags, 'Generated ESP must be ESL flagged without the Master flag')
    bindings_path = 'F4SE/Plugins/DIAMOND/Generated/'+stem+'/objects.json'
    bindings = dict(format='diamond-object-bindings', version=2, plugin=plugin, sourcePlugin=master,
                    installation=installation, packs=sorted(p['id'] for p in parts),
                    objects={key: dict(form=f'{plugin}:{entry["localId"]:06x}', editorId=entry['editorId'],
                                       model=entry['definition']['model'][7:], name=entry['definition']['name'], active=True)
                             for key, entry in group['objects'].items() if entry['active']})
    write(output/bindings_path, bindings)
    files = {plugin, bindings_path}
    for pack in emitted:
        files.update(pack['manifest']['files'])
        files.add('F4SE/Plugins/DIAMOND/Packs/'+pack['manifest']['id']+'/manifest.json')
    write(output/'F4SE/Plugins/DIAMOND/Generated'/stem/'files.json',
          dict(plugin=plugin, sourcePlugin=master, files=sorted(files),
               modelOverrides=sorted({path for p in emitted for path in p['manifest'].get('modelOverrides', [])})))
    return group, dict(plugin=plugin, master=master, objects=len(bindings['objects'])), len(bindings['objects'])


README = ('Install this bundle as one MO2 mod. Enable only the ESL-flagged ESPs for the original source mods you use.\n'
          'Requires DIAMOND with source-plugin ESL support. Fallout 4 VR also requires an ESL loader such as Daytripper.\n'
          'Generated magazine meshes are grouped under each plugin name. Weapon model overrides retain their required native paths.\n'
          'Source plugins, activation and load order are not modified by the exporter.\n')


def scope_profile_path(model):
    """Where True Scopes reads an optic's profile; the name is the model's
    file name and a short digest of its path, as Maker's own export."""
    base = PurePosixPath(model.replace('\\', '/')).name.rsplit('.', 1)[0]
    name = re.sub(r'[^a-zA-Z0-9_-]', '_', base)
    return 'F4SE/Plugins/TrueScopesVR/Scopes/'+name+'-'+hashlib.sha256(model.encode('utf-8')).hexdigest()[:12]+'.json'


async def export_library(workspace, selections, backends, archive=None, mo2=None, loaded=None, scopes=False):
    """Exports chosen saved versions of ESP projects as one bundle.

    Each selection names an ESP project and optionally a saved version (the
    latest by default). Only committed authoring is read. Every weapon of the
    version with saved reload behavior is exported; a weapon without it is
    reported and the others continue. With `loaded` (weapon records of the
    source Maker has open), weapons outside it are reported and skipped. With
    `scopes`, the True Scopes profiles of the exported weapons join the same
    bundle. Object IDs come from each ESP's own export identity and are
    recorded only after the bundle is delivered."""
    import product_library as library
    from product_export import export_pack
    require(bool(archive) != bool(mo2), 'Choose Export to MO2 or Export ZIP / 7z')
    workspace = Path(workspace).resolve()
    require(isinstance(selections, list) and 0 < len(selections) <= 256, 'Select between one and 256 ESP projects')
    loaded = None if loaded is None else {library.weapon_key(record) for record in loaded}
    chosen = {}
    for selection in selections:
        key = library.plugin_key(selection.get('plugin'))
        require(key not in chosen, 'An ESP project is selected twice: '+selection['plugin'])
        folder = library.library_root(workspace)/key
        project = library.read_project(folder)
        require(project is not None and project['saves'], 'No saved versions for '+selection['plugin'])
        save = selection.get('save') or project['latest']
        require(any(s['id'] == save for s in project['saves']), 'Unknown saved version '+str(save)+' of '+project['plugin'])
        chosen[key] = dict(folder=folder, project=project, save=save)
    stage = workspace/'local/tmp/library-export'/uuid.uuid4().hex
    with contextlib.ExitStack() as locks:
        for key in sorted(chosen):
            locks.enter_context(library.locked(chosen[key]['folder']))
        try:
            output = stage/'output'
            output.mkdir(parents=True)
            capability = backends.content_validation()
            require(capability.get('sourcePluginLightESP') is True, 'Update DIAMOND and its bundled validator for ESL-flagged ESP exports')
            limits = capability['limits']
            packs, skipped, warnings, optics = [], [], [], {}
            reused_weapons = reused_groups = 0
            for key, item in sorted(chosen.items()):
                snapshot = library.read_snapshot(item['folder'], item['save'])
                item['weapons'] = []
                item['exportInputs'] = {}
                for record, entry in sorted(snapshot['weapons'].items()):
                    cancellation()
                    name = entry.get('name', record)
                    if loaded is not None and library.weapon_key(record) not in loaded:
                        skipped.append(dict(plugin=snapshot['plugin'], record=record, name=name, reason='not in the loaded source'))
                        continue
                    if scopes and entry.get('authoring'):
                        authoring = library.get_record(item['folder'], entry['authoring'])
                        for profile in (authoring.get('scopeProfiles') or {}).values():
                            path = scope_profile_path(profile['model'])
                            if path not in optics:
                                optics[path] = profile
                            elif optics[path] != profile:
                                warnings.append(dict(record=record, name=name, warning='another weapon already exported a different profile for '+profile['model']))
                    if not entry.get('pack'):
                        skipped.append(dict(plugin=snapshot['plugin'], record=record, name=name, reason='no saved reload behavior'))
                        continue
                    try:
                        source = library.materialize_pack(item['folder'], entry['pack'], stage/'inputs'/key/record.split(':')[1]/'pack.project.json')
                        frozen = stage/'frozen'/str(len(packs))
                        inputs = library.get_record(item['folder'], entry['pack'])
                        cache = item['folder']/'export/cache/weapons'/record.split(':')[1]
                        if restore_export(cache, inputs, frozen) is None:
                            export_pack(str(source), str(frozen), backends)
                            remember_export(cache, inputs, frozen, {})
                        else:
                            reused_weapons += 1
                        packs.append(load_pack(frozen))
                        item['exportInputs'][record] = inputs
                    except ValueError as error:
                        skipped.append(dict(plugin=snapshot['plugin'], record=record, name=name, reason=str(error)))
                        continue
                    item['weapons'].append(record)
                    progress = (entry.get('status') or {}).get('progress')
                    if progress and progress != 'complete':
                        warnings.append(dict(record=record, name=name, warning='checklist not complete ('+progress+')'))
                    failed = (entry.get('packReport') or {}).get('failed')
                    if failed:
                        warnings.append(dict(record=record, name=name, warning=f'{failed} configuration(s) kept their earlier behavior at the last save'))
            require(packs or optics, 'Nothing to export: the selected versions contain no saved reload behavior'+(' or scope profiles' if scopes else ''))
            require(len(packs) <= limits['packs'], 'Too many selected reload packs')
            plugins, identities, count, checked = [], {}, 0, None
            if packs:
                groups = partition(packs)
                require(set(groups) <= set(chosen), 'A saved weapon does not belong to its ESP project')
                native = backends.adapter('nif'); validated = set()
                backend = backends.adapter('plugin')
                for master, parts in sorted(groups.items()):
                    cancellation()
                    folder = chosen[master]['folder']
                    value = library.read_identity(folder) or library.new_identity(master)
                    cache = folder/'export/cache/group'
                    inputs = dict(weapons=chosen[master]['exportInputs'], identity=value)
                    prepared = stage/'groups'/master
                    cached = restore_export(cache, inputs, prepared)
                    if cached is None:
                        group, row, objects = await emit_group(master, parts, value.get('group'), value['installation'],
                                                               prepared, stage, backend, native, validated)
                        value['group'] = group
                        # Allocation is part of the reusable result. Compare the
                        # next request against that resulting permanent identity.
                        inputs['identity'] = value
                        remember_export(cache, inputs, prepared, dict(group=group, row=row, objects=objects))
                    else:
                        group, row, objects = cached['group'], cached['row'], cached['objects']
                        reused_groups += 1
                    for path in prepared.rglob('*'):
                        if path.is_file():
                            copy_file(path, output/path.relative_to(prepared))
                    value['group'] = group
                    identities[master] = value
                    plugins.append(dict(row, save=chosen[master]['save']))
                    count += objects
                require(count <= limits['worldObjects'], 'Selected active objects exceed DIAMOND runtime capacity')
                (output/'DIAMOND Reload Packs.txt').write_text(README, encoding='utf-8')
                checked = backends.content_validation(output)
                require(checked.get('ok') and checked.get('objects') == count, 'DIAMOND rejected grouped content: '+json.dumps(checked))
            for path, profile in sorted(optics.items()):
                write(output/path, profile)
            cancellation()
            delivered = deliver(output, archive, mo2)
            # The IDs are permanent from the moment the bundle is delivered.
            moment = library.iso(library.now())
            for master, value in identities.items():
                folder = chosen[master]['folder']
                write(folder/'export/identity.json', value)
                published = library.read_published(folder)
                destination = dict(kind='mo2', path=delivered['installedTo']) if mo2 else dict(kind='archive', path=delivered['archive'])
                published['publications'].append(dict(save=chosen[master]['save'], exportedAt=moment, destination=destination,
                                                      plugin=plugin_name(master), weapons=chosen[master]['weapons'],
                                                      objects=sum(1 for e in value['group']['objects'].values() if e['active'])))
                published['publications'] = published['publications'][-200:]
                write(folder/'export/published.json', published)
            return dict(ok=True, plugins=plugins, activeObjects=count, scopeProfiles=len(optics), skipped=skipped, warnings=warnings,
                        runtimeCatalog=checked, reusedWeapons=reused_weapons, reusedGroups=reused_groups, **delivered)
        finally:
            shutil.rmtree(stage, ignore_errors=True)


