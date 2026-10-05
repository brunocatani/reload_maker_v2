"""Publish an authored reload project as a portable Data-layout pack.

The project owns paths to prepared behavior and finished objects. The publisher
freezes those inputs, checks native collision, and writes no original plugin.
"""
import copy
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid

from product_packs import (TEMPLATES, cancellation, digest, identity, load_pack,
                           read, relative, require, within, write)


def form_key(value):
    require(isinstance(value, str) and re.fullmatch(r'[^:/\\\x00]+\.(?:esp|esm|esl):[0-9a-f]{6}', value, re.I),
            'Expected an originating plugin and six-digit local record ID')
    return value.lower()


def project_source(project, entry):
    require(isinstance(entry, dict), 'Missing frozen project input')
    path = Path(entry['path'])
    path = path if path.is_absolute() else project.parent/path
    path = path.resolve()
    require(path.is_file(), f'Missing authored input: {path}')
    return path


def inspect_project(path, skip=frozenset()):
    """Check a pack project. Weapons in `skip` (lower-case records) are left
    out of an export: their inputs are not read and they are not summarized,
    but they still own their object identities."""
    project = read(path)
    require(project.get('format') == 'reload-maker-pack-project' and project.get('version') == 1,
            'Select a Reload Maker pack project')
    pack = identity(project.get('id'))
    require('/' not in pack and len(pack) <= 128, 'Pack ID must be one namespace')
    require(type(project.get('revision')) is int and project['revision'] > 0, 'Invalid pack revision')
    weapons = project.get('weapons')
    require(isinstance(weapons, list) and 0 < len(weapons) <= 32, 'Select at least one authored weapon')
    objects = project.get('objects')
    require(isinstance(objects, list) and 0 < len(objects) <= 256, 'Missing finished magazine objects')
    claims, identifiers, included = set(), set(), set()
    summary = []
    for weapon in weapons:
        key = form_key(weapon.get('record'))
        require(key not in claims, 'One project weapon may contain several configurations; do not duplicate its record')
        claims.add(key)
        if key in skip:
            for assignment in weapon.get('magazineObjects', []):
                require(assignment['full'] not in identifiers and assignment['empty'] not in identifiers,
                        'Different weapon/magazine fits need distinct logical object identities')
                identifiers.update((assignment['full'], assignment['empty']))
            continue
        require(weapon.get('template') in TEMPLATES, 'Unsupported reload template')
        documents = {kind: read(project_source(path, weapon[kind])) for kind in ('profiles', 'magazines', 'grabs', 'sounds')}
        profile, magazines = documents['profiles'], documents['magazines']
        require(form_key(profile['weapon']) == key and form_key(magazines['weapon']) == key,
                'Prepared behavior belongs to a different weapon')
        require(magazines['version'] == 3, 'Magazine export must use logical object bindings, without local world forms')
        if weapon['template'] != 'pistol.slide-detachable.v1':
            require(all('rightGrip' in m for m in magazines['magazines']), 'Rifle magazines need both authored hand fits')
        records = {form_key(item['record']) for item in magazines['magazines']}
        require(records == {form_key(item['magazine']) for item in profile['profiles']}, 'Magazine/configuration coverage differs')
        assignments = weapon.get('magazineObjects', [])
        require(len(assignments) == len(records) and {form_key(a['record']) for a in assignments} == records,
                'Every authored magazine needs one full and one empty object')
        for assignment in assignments:
            require(assignment['full'] != assignment['empty'], 'Full and empty need independent object identities')
            require(assignment['full'] not in identifiers and assignment['empty'] not in identifiers,
                    'Different weapon/magazine fits need distinct logical object identities')
            identifiers.update((assignment['full'], assignment['empty']))
            included.update((assignment['full'], assignment['empty']))
        summary.append(dict(record=key, name=weapon.get('name', key), template=weapon['template'],
                            variants=len(profile['profiles']), magazines=len(records)))
    require(summary, 'No weapon in this project is ready to export; complete a checklist first')
    object_ids = [obj.get('id') for obj in objects]
    require(len(set(object_ids)) == len(object_ids) and set(object_ids) == identifiers,
            'Object definitions must match the complete current magazine assignments')
    for obj in objects:
        require(identity(obj['id']).startswith(pack+'/'), 'Object is outside the project namespace')
        if obj['id'] in included:
            project_source(path, obj['source'])
    return project, summary


def export_pack(path, destination, backends, skip=()):
    """Publish the project's weapons except those in `skip` (the editor's
    weapons whose checklist is not complete), with only their objects."""
    path, destination = Path(path).resolve(), Path(destination).resolve()
    skip = frozenset(form_key(record) for record in skip)
    project, summary = inspect_project(path, skip)
    exported = [weapon for weapon in project['weapons'] if form_key(weapon['record']) not in skip]
    owned = {a[state] for weapon in exported for a in weapon['magazineObjects'] for state in ('full', 'empty')}
    # Each publication is immutable. A failed export never replaces a previous
    # valid directory or mixes it with current authoring files.
    require(not destination.exists(), 'Choose a new export folder; finished exports are immutable')
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = destination.parent/('.'+destination.name+'.'+uuid.uuid4().hex+'.pending')
    stage.mkdir()
    pack = project['id']
    prefix = f'F4SE/Plugins/DIAMOND/Packs/{pack}/'
    files, objects, weapons, materials = [], [], [], set()
    native = backends.adapter('nif')
    verified = set()
    try:
        for weapon in exported:
            cancellation()
            key = form_key(weapon['record'])
            plugin, local = key.split(':')
            folder = prefix+f'weapons/{plugin}/{local}/'
            item = dict(record=key, name=weapon.get('name', key), template=weapon['template'],
                        magazineObjects=copy.deepcopy(weapon['magazineObjects']))
            for kind in ('profiles', 'magazines', 'sounds', 'grabs'):
                source = project_source(path, weapon[kind])
                target = folder+kind+'.json'
                within(stage, target).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, within(stage, target))
                files.append(target);item[kind] = target
            validator = backends.application/'backends/diamond/DiamondProfileCheck.exe'
            require(validator.is_file(), 'The bundled DIAMOND content validator is missing')
            # The Data namespace plus an author-chosen workspace can exceed
            # Win32 MAX_PATH. Pass explicit extended absolute paths to the
            # native checker; do not shorten or flatten the authored layout.
            native_path = lambda value: ('\\\\?\\'+str(value)) if os.name == 'nt' else str(value)
            checked = subprocess.run([str(validator), '--authored', *[native_path(within(stage, item[k]))
                for k in ('profiles', 'magazines', 'sounds', 'grabs')]], capture_output=True, text=True,
                timeout=60, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            require(checked.returncode == 0, 'DIAMOND rejected authored content: '+(checked.stderr or checked.stdout).strip())
            weapons.append(item)
        for obj in project['objects']:
            cancellation()
            if obj['id'] not in owned:
                continue
            source = project_source(path, obj['source'])
            # Identical finished models share one file named by their content.
            fingerprint = digest(source)
            if fingerprint not in verified:
                validation, info = native.nif_validate(str(source)), native.nif_info(str(source))
                data = info.get('data', {});types = data.get('blockTypes', {})
                require(validation.get('ok') and info.get('ok') and data.get('bsVersion') == 130 and
                        types.get('bhkNPCollisionObject') == 1 and types.get('bhkPhysicsSystem') == 1,
                        f'Object needs a validated, cooked Fallout 4 collision model: {source}')
                blocks = native.nif_blocks(str(source))
                require(blocks.get('ok'), 'Cannot inspect finished model material dependencies')
                for block in blocks['data']:
                    if block['type'] in ('BSLightingShaderProperty', 'BSEffectShaderProperty') and block.get('name'):
                        materials.add(relative(block['name']))
                verified.add(fingerprint)
            model = f'Meshes/DIAMOND/Packs/{pack}/{fingerprint}.nif'
            target = within(stage, model);target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists():
                shutil.copy2(source, target);files.append(model)
            objects.append({**{k: copy.deepcopy(obj[k]) for k in ('id', 'name', 'bounds', 'weight', 'value')}, 'model': model})
            materials.update(relative(value) for value in obj.get('materialDependencies', []))
        object_path = prefix+'objects.json';write(within(stage, object_path), objects);files.append(object_path)
        manifest = dict(format='diamond-content-pack', version=1, id=pack, revision=project['revision'],
                        name=project.get('name', pack), templates=sorted({w['template'] for w in weapons}),
                        files=sorted(files), objects=object_path, weapons=weapons,
                        requirements=copy.deepcopy(project.get('requirements', {})))
        manifest['requirements']['materials'] = sorted(materials)
        write(stage/prefix/'manifest.json', manifest)
        load_pack(stage)
        # Recheck the authoring input after all native jobs, not merely before.
        inspected, final_summary = inspect_project(path, skip)
        require(inspected == project and final_summary == summary, 'Project changed during export')
        write(stage/'export-report.json', dict(format='reload-maker-export-report', version=1,
            pack=pack, revision=project['revision'], weapons=summary, objects=len(objects),
            uniqueModels=len(verified), nativeCollisionValidated=True,
            gameTested=False, requirements=manifest['requirements'], skippedWeapons=sorted(skip)))
        cancellation()
        os.replace(stage, destination)
        return dict(ok=True, output=str(destination), manifest=str(destination/prefix/'manifest.json'),
                    weapons=len(weapons), skipped=sorted(skip), variants=sum(w['variants'] for w in summary),
                    magazines=sum(w['magazines'] for w in summary), objects=len(objects))
    except Exception:
        # The uniquely created staging directory is the only cleanup target.
        if stage.parent == destination.parent and stage.name.endswith('.pending'):
            shutil.rmtree(stage)
        raise
