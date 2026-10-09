"""ESP project library: explicit saves, dated versions, deletion, migration
and changed-files-only MO2 delivery. Offline; no editor or native backend.

Usage: esp_library_test.py <scratch folder>
"""
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import product_library as library
from product_grouped import deliver
from product_library_migrate import migrate
from product_packs import read, write

PLUGIN = 'Test_Rifle.esp'
FIRST, SECOND = 'Test_Rifle.esp:000801', 'Test_Rifle.esp:000802'


def weapon_record(workspace, record, marker, folder='requests'):
    value = dict(format=1, kind='reload-maker-v2-project', weapon=record, weaponName='Rifle '+record[-3:],
                 package='D:/somewhere/testbed', gameData='D:/Data', library=False, mo2Instance='', mo2Profile='',
                 reloadTemplates={record.lower(): 'rifle.ar15-detachable.v1'}, authoringRoadmaps={'marker': {'note': marker}})
    path = workspace/folder/(record.replace(':', '-')+'-'+marker+'.json')
    write(path, value)
    return path


def save(workspace, base, weapons, note=''):
    request = dict(format='reload-maker-library-save-request', version=1, plugin=PLUGIN, base=base,
                   weapons={r.lower(): dict(name='Rifle', authoring=str(p)) for r, p in weapons.items()},
                   source=dict(mode='package', package='D:/somewhere/testbed', gameData='D:/Data'), note=note)
    path = workspace/'requests'/'request.json'
    write(path, request)
    return library.save_request(workspace, path, backends=None)


def latest(workspace):
    return library.read_project(library.project_folder(workspace, PLUGIN))['latest']


def authoring(workspace, save_id, record):
    folder = library.project_folder(workspace, PLUGIN)
    snapshot = library.read_snapshot(folder, save_id)
    return library.get_record(folder, snapshot['weapons'][record.lower()]['authoring'])


def expect_failure(action, text):
    try:
        action()
    except (ValueError, RuntimeError) as error:
        assert text in str(error), error
        return
    raise AssertionError('expected failure: '+text)


def library_versions(workspace):
    first = save(workspace, '', {FIRST: weapon_record(workspace, FIRST, 'a')})['save']['id']
    assert latest(workspace) == first
    record = authoring(workspace, first, FIRST)
    assert record['kind'] == library.AUTHORING_KIND and 'package' not in record, 'source location must be metadata only'
    snapshot = library.read_snapshot(library.project_folder(workspace, PLUGIN), first)
    assert snapshot['weapons'][FIRST.lower()]['source']['package'] == 'D:/somewhere/testbed'
    # A save of one weapon keeps the other weapons of the ESP.
    second = save(workspace, first, {SECOND: weapon_record(workspace, SECOND, 'b')})['save']['id']
    assert latest(workspace) == second and authoring(workspace, second, FIRST)['authoringRoadmaps']['marker']['note'] == 'a'
    # Same-second saves stay distinct and ordered by sequence.
    assert second != first and second.endswith('_0002')
    # Adjusting an older version saves a new latest and keeps the older one.
    third = save(workspace, first, {FIRST: weapon_record(workspace, FIRST, 'c')})['save']['id']
    assert latest(workspace) == third and SECOND.lower() not in library.read_snapshot(library.project_folder(workspace, PLUGIN), third)['weapons']
    assert authoring(workspace, first, FIRST)['authoringRoadmaps']['marker']['note'] == 'a'
    # A failed or cancelled save changes nothing.
    before = sorted(p.name for p in (library.project_folder(workspace, PLUGIN)/'saves').iterdir())
    expect_failure(lambda: save(workspace, third, {SECOND: weapon_record(workspace, FIRST, 'x')}), 'another record')
    cancel = workspace/'cancel.flag'
    cancel.write_text('stop')
    os.environ['RELOAD_MAKER_CANCEL_FILE'] = str(cancel)
    try:
        expect_failure(lambda: save(workspace, third, {FIRST: weapon_record(workspace, FIRST, 'y')}), 'cancelled')
    finally:
        del os.environ['RELOAD_MAKER_CANCEL_FILE']
    assert latest(workspace) == third
    assert sorted(p.name for p in (library.project_folder(workspace, PLUGIN)/'saves').iterdir()) == before
    # Deleting the latest makes the next newest latest; identity survives all deletes.
    identity = dict(library.new_identity(PLUGIN), group=dict(version=1, plugin='DIAMOND_test_rifle.esp', nextId=0x803,
                    objects={'p/magazine/a/full': dict(localId=0x801), 'p/magazine/a/empty': dict(localId=0x802)}))
    write(library.project_folder(workspace, PLUGIN)/'export/identity.json', identity)
    assert library.delete_save(workspace, PLUGIN, third)['latest'] == second
    expect_failure(lambda: save(workspace, third, {FIRST: weapon_record(workspace, FIRST, 'z')}), 'was deleted')
    library.delete_save(workspace, PLUGIN, second)
    library.delete_save(workspace, PLUGIN, first)
    assert latest(workspace) is None
    assert read(library.project_folder(workspace, PLUGIN)/'export/identity.json') == identity
    print('PASS dated versions: explicit saves, sibling retention, older-version adjustment, failed/cancelled saves, deletion and identity')


def migration(workspace):
    legacy = workspace/'workspace/projects/test_rifle.esp/000801/0123456789abcdef'
    document = json.loads(weapon_record(workspace, FIRST, 'legacy').read_text(encoding='utf-8'))
    write(legacy/'authoring.project.json', document)
    write(legacy/'status.json', dict(format='reload-maker-weapon-status', version=1, weapon=FIRST,
                                     configurations={'x': dict(progress='complete')}))
    blank = dict(document, reloadTemplates={}, authoringRoadmaps={})
    write(workspace/'workspace/projects/test_rifle.esp/000801/fedcba9876543210/authoring.project.json', blank)
    pack = workspace/'workspace/packs/test_rifle.esp/000801/0123456789abcdef'
    for kind in ('profiles', 'magazines', 'sounds', 'grabs'):
        write(pack/'authoring/one'/(kind+'.json'), dict(kind=kind))
    (pack/'models').mkdir(parents=True)
    (pack/'models/full.nif').write_bytes(b'full model')
    write(pack/'models/full.receipt.json', dict(engineCooked=True))
    write(pack/'pack.project.json', dict(format='reload-maker-pack-project', version=1, id='reload.test.000801', revision=3,
          weapons=[dict(record=FIRST, template='rifle.ar15-detachable.v1', captures=[{}], magazineObjects=[],
                        **{k: dict(path='authoring/one/'+k+'.json') for k in ('profiles', 'magazines', 'sounds', 'grabs')})],
          objects=[dict(id='reload.test.000801/magazine/x/full', source=dict(path='models/full.nif'))]))
    generation = workspace/'workspace/installations/source-esl/generations/g'
    write(generation/'ledger.json', dict(version=2, installation='inst', groups={'test_rifle.esp': dict(
        version=1, plugin='DIAMOND_test_rifle.esp', nextId=0x802, generation='generations/g',
        objects={'reload.test.000801.diamond_test_rifle/magazine/x/full': dict(localId=0x801)})}))
    write(generation/'output/F4SE/Plugins/DIAMOND/Packs/reload.test.000801.diamond_test_rifle/manifest.json',
          dict(id='reload.test.000801.diamond_test_rifle', authoringId='reload.test.000801', revision=3, weapons=[dict(record=FIRST)]))
    write(workspace/'workspace/installations/source-esl/current.json', dict(version=2, generation='generations/g'))
    plan = migrate(workspace)
    assert not plan['applied'] and not (workspace/'workspace/library').exists(), 'a dry run must not write'
    report = migrate(workspace, apply=True)
    folder = library.project_folder(workspace, PLUGIN)
    project = library.read_project(folder)
    assert len(project['saves']) == 1 and project['saves'][0]['origin'] == 'migration'
    snapshot = library.read_snapshot(folder, project['latest'])
    entry = snapshot['weapons'][FIRST.lower()]
    assert entry['status']['progress'] == 'complete' and entry['pack']
    stored = library.get_record(folder, entry['pack'])
    assert stored['id'] == 'reload.test.000801' and stored['objects'][0]['source']['path'].startswith('assets/')
    assert (folder/stored['objects'][0]['source']['path']).with_suffix('.receipt.json').is_file(), 'cook receipt kept beside the model'
    value = read(folder/'export/identity.json')
    assert value['installation'] == 'inst' and value['group']['objects']['reload.test.000801.diamond_test_rifle/magazine/x/full']['localId'] == 0x801
    assert 'generation' not in value['group'] and value['packIds'][FIRST.lower()] == 'reload.test.000801'
    assert any('blank draft' in i['reason'] for i in report['ignored'])
    assert (legacy/'authoring.project.json').is_file(), 'legacy files stay'
    again = migrate(workspace, apply=True)
    assert again['skippedProjects'] and len(library.read_project(folder)['saves']) == 1, 'a repeated import never duplicates'
    print('PASS migration: dry run, chosen project, blank drafts, pinned assets, carried IDs, untouched legacy files')


def delivery(workspace):
    output, mod = workspace/'out', workspace/'mods/Reloads'
    receipt = 'F4SE/Plugins/DIAMOND/Generated/DIAMOND_a/files.json'
    write(output/receipt, dict(plugin='DIAMOND_a.esp', files=['DIAMOND_a.esp', 'Meshes/DIAMOND_a/x.nif'], modelOverrides=[]))
    (output/'DIAMOND_a.esp').write_bytes(b'plugin')
    (output/'Meshes/DIAMOND_a').mkdir(parents=True)
    (output/'Meshes/DIAMOND_a/x.nif').write_bytes(b'mesh')
    mod.mkdir(parents=True)
    (mod/'unrelated.txt').write_text('keep')
    first = deliver(output, mo2=mod)
    assert first['written'] == 3 and first['unchanged'] == 0
    stamp = (mod/'DIAMOND_a.esp').stat().st_mtime_ns
    again = deliver(output, mo2=mod)
    assert again['written'] == 0 and again['unchanged'] == 3 and (mod/'DIAMOND_a.esp').stat().st_mtime_ns == stamp
    (output/'Meshes/DIAMOND_a/x.nif').write_bytes(b'mesh changed')
    changed = deliver(output, mo2=mod)
    assert changed['written'] == 1 and (mod/'Meshes/DIAMOND_a/x.nif').read_bytes() == b'mesh changed'
    assert (mod/'unrelated.txt').read_text() == 'keep'
    print('PASS MO2 delivery: identical files left in place, changed files replaced, unrelated files kept')


def shared_models(workspace):
    from product_authoring import presentation_identity
    setup = dict(recipeId='old', trackAnchors=[dict(path='receiver/old')], pieces=[dict(path='mag/body', role='body')])
    same = dict(setup, recipeId='new', trackAnchors=[dict(path='receiver/new')])
    assert presentation_identity(setup) == presentation_identity(same), 'Capture bookkeeping invalidated magazine models'
    changed = dict(same, pieces=[dict(path='mag/body', role='cartridge')])
    assert presentation_identity(setup) != presentation_identity(changed), 'Physical payload changes lost their model validation'
    folder = library.project_folder(workspace, PLUGIN)
    pair, references = {}, {}
    for presentation in ('full', 'empty'):
        # Only file routing/receipt presence is under test; no model parser.
        model = workspace/'inputs'/(presentation+'.bin')
        model.parent.mkdir(parents=True, exist_ok=True)
        model.write_bytes(('opaque '+presentation+' test asset').encode())
        write(model.with_suffix('.receipt.json'), dict(engineCooked=True))
        references[presentation] = library.put_asset(folder, model)
        pair[presentation] = str(library.resolve(folder, references[presentation]).resolve())
        write(Path(pair[presentation]).with_suffix('.receipt.json'), dict(engineCooked=True))
    magazine = 'test_rifle.esp:000900'
    pack = dict(id='reload.test.models', weapons=[dict(record=FIRST, magazineObjects=[dict(record=magazine.upper(), full='full', empty='empty')])],
                objects=[dict(id=p, source=dict(path=references[p])) for p in pair])
    reference = library.put_record(folder, pack)
    models = library.saved_magazine_models(folder, {FIRST: dict(pack=reference), SECOND: {}})
    assert models == {magazine: pair}, models
    # Imported/current assignments beat both persisted and newly authored
    # sibling defaults. Only this weapon's explicit assignment replaces them.
    from unittest.mock import patch
    captures = workspace/'captures'
    write(captures/'0000.json', {})
    request = dict(captures=str(captures), sharedModels={magazine: pair})
    stage = workspace/'staged'
    with patch('product_authoring.save', return_value=dict(saved=0, failed=0, rows=[])) as capture_save:
        library.save_weapon_pack(folder, PLUGIN, FIRST, dict(pack=reference), request,
                                 dict(packIds={}), stage, None, models)
        assert capture_save.call_args.kwargs['models'] == {}, 'Sibling default replaced an existing custom assignment'
        request['models'] = {magazine: pair}
        library.save_weapon_pack(folder, PLUGIN, FIRST, dict(pack=reference), request,
                                 dict(packIds={}), workspace/'staged-explicit', None, models)
        assert capture_save.call_args.kwargs['models'] == {magazine: (pair['full'], pair['empty'])}, 'Explicit replacement lost precedence'
    # A missing pair cannot masquerade as usable cooked models.
    Path(pair['empty']).with_suffix('.receipt.json').unlink()
    assert library.saved_magazine_models(folder, {FIRST: dict(pack=reference)}) == {}
    print('PASS sibling model lookup: imported pack assignments, canonical magazine IDs, complete receipt pairs')


def run(scratch):
    scratch.mkdir(parents=True, exist_ok=True)
    for test in (library_versions, migration, delivery, shared_models):
        with tempfile.TemporaryDirectory(dir=scratch) as folder:
            test(Path(folder))


if __name__ == '__main__':
    run(Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]/'local/esp-library-tests')
