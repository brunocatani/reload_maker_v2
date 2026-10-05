"""Stable IDs, selected-pack output and failed-build preservation using native backends."""
import argparse
import asyncio
import copy
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from product_backend import Backends
from product_packs import allocate, build, load_pack, PLUGIN, read, write


async def run(args):
    workspace = Path(args.workspace).resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    backends = Backends(Path(args.application), workspace)
    with tempfile.TemporaryDirectory(prefix='pack-build-', dir=workspace) as temporary:
        root = Path(temporary)
        packs = []
        for i, source in enumerate(args.pack):
            destination = root/f'pack{i}'
            shutil.copytree(source, destination)
            packs.append(destination)
        assert len(packs) >= 2, 'Supply two independent finished pack fixtures'
        install = root/'installation'
        first = await build(packs, install, backends, initialize=True)
        assert first['runtimeCatalog']['ok'] and first['runtimeCatalog']['objects']==first['activeObjects']
        pointer = read(install/'current.json')
        ledger = read(install/pointer['generation']/'ledger.json')
        ids = {k: v['localId'] for k, v in ledger['objects'].items()}
        second = await build(list(reversed(packs)), install, backends)
        reordered = read(install/read(install/'current.json')['generation']/'ledger.json')
        assert ids == {k: v['localId'] for k, v in reordered['objects'].items()}
        plugin = lambda built: (install/built['generation']['generation']/'output'/PLUGIN).read_bytes()
        assert plugin(first) == plugin(second), 'Pack order changed the generated plugin'
        removed = await build(packs[:1], install, backends)
        assert removed['activeObjects'] < len(ids)
        retained = read(install/read(install/'current.json')['generation']/'ledger.json')
        assert any(not entry['active'] for entry in retained['objects'].values())
        assert ids == {k: v['localId'] for k, v in retained['objects'].items()}
        output=Path(removed['output'])
        bindings=read(output/'F4SE/Plugins/DIAMOND/Generated/objects.json')['objects']
        omitted={key for key, value in retained['objects'].items() if not value['active']}
        assert omitted.isdisjoint(bindings)
        for key in omitted:
            assert not (output/ledger['objects'][key]['definition']['model']).exists()
            assert 'definition' not in retained['objects'][key]
        original_pointer = (install/'current.json').read_bytes()
        loaded=load_pack(packs[0]);manifest_path=loaded['manifestPath']
        objects = packs[0]/loaded['manifest']['objects']
        original = objects.read_bytes()
        objects.write_bytes(b'[]')
        try:
            await build(packs, install, backends)
            raise AssertionError('An empty object list was accepted')
        except ValueError as exc:
            assert 'object count' in str(exc), str(exc)
        assert (install/'current.json').read_bytes() == original_pointer
        objects.write_bytes(original)
        manifest_bytes=manifest_path.read_bytes()
        malformed=copy.deepcopy(loaded['manifest']);malformed['weapons'][0]['template']='unsupported.behavior'
        write(manifest_path,malformed)
        try:
            await build(packs[:1],install,backends)
            raise AssertionError('Semantically invalid content reached the active generation')
        except ValueError as exc:
            assert 'complete generated catalog' in str(exc),str(exc)
        assert (install/'current.json').read_bytes()==original_pointer
        manifest_path.write_bytes(manifest_bytes)
        budget=backends.content_validation()['limits']['worldObjects']
        oversized=[dict(loaded['objects'][0],id=loaded['manifest']['id']+'/test-'+str(i)) for i in range(budget)]
        write(objects,oversized)
        try:
            await build(packs,install,backends)
            raise AssertionError('An aggregate object catalog above runtime capacity was accepted')
        except ValueError as exc:
            assert 'capacity' in str(exc),str(exc)
        assert (install/'current.json').read_bytes()==original_pointer
        objects.write_bytes(original);manifest_path.write_bytes(manifest_bytes)
        incomplete=copy.deepcopy(loaded['manifest']);incomplete.pop('weapons');write(manifest_path,incomplete)
        try:
            load_pack(packs[0]);raise AssertionError('Object-only pack was accepted as a physical reload')
        except ValueError as exc:
            assert 'object-only' in str(exc),str(exc)
        manifest_path.write_bytes(manifest_bytes)
        bad = copy.deepcopy(retained)
        bad['nextId'] = 0x800
        try:
            allocate(bad, [load_pack(packs[0])])
            raise AssertionError('Corrupt allocation accepted')
        except ValueError as exc:
            assert 'high-water' in str(exc)
        active_generation = install/read(install/'current.json')['generation']
        saved_ledger = (active_generation/'ledger.json').read_bytes()
        (active_generation/'ledger.json').write_bytes(b'{}')
        try:
            await build(packs, install, backends)
            raise AssertionError('Corrupt established ledger was replaced')
        except ValueError as exc:
            assert 'corrupted' in str(exc)
        assert (install/'current.json').read_bytes() == original_pointer
        (active_generation/'ledger.json').write_bytes(saved_ledger)
        bindings_path=active_generation/'output/F4SE/Plugins/DIAMOND/Generated/objects.json'
        saved_bindings=bindings_path.read_bytes();bindings_path.write_bytes(b'{}')
        try:
            await build(packs,install,backends)
            raise AssertionError('Changed last-good bindings were accepted')
        except ValueError as exc:
            assert 'bindings' in str(exc),str(exc)
        assert (install/'current.json').read_bytes()==original_pointer
        bindings_path.write_bytes(saved_bindings)
        restored = await build(packs, install, backends)
        assert restored['activeObjects'] == len(ids)
        # Cancel after native catalog validation on the very first generation.
        # A retry must reuse its seed and must not demand a nonexistent pointer.
        fresh=root/'cancelled-initial-installation';marker=root/'late-cancel'
        original_validation=backends.content_validation
        def cancel_after_catalog(data=None):
            checked=original_validation(data)
            if data is not None:marker.write_text('cancel',encoding='utf-8')
            return checked
        backends.content_validation=cancel_after_catalog
        old_marker=os.environ.get('RELOAD_MAKER_CANCEL_FILE')
        os.environ['RELOAD_MAKER_CANCEL_FILE']=str(marker)
        try:
            try:
                await build(packs[:1],fresh,backends,initialize=True)
                raise AssertionError('Late first-build cancellation promoted an output')
            except RuntimeError as exc:
                assert 'cancelled' in str(exc),str(exc)
            assert not (fresh/'current.json').exists()
            assert not list((fresh/'generations').glob('*/receipt.json'))
            seed=(fresh/'identity.json').read_bytes()
        finally:
            backends.content_validation=original_validation
            if old_marker is None:os.environ.pop('RELOAD_MAKER_CANCEL_FILE',None)
            else:os.environ['RELOAD_MAKER_CANCEL_FILE']=old_marker
        retry=await build(packs[:1],fresh,backends,initialize=True)
        assert retry['ok'] and (fresh/'identity.json').read_bytes()==seed
        report = dict(ok=True, objects=len(ids), checks=['native cooked NIF validation', 'ESP independent readback',
            'stable reordered IDs and plugin bytes', 'removed packs emit no records or models', 'tamper failure preserves output',
            'corrupt ledger rejected', 'reinstall recovers existing IDs', 'runtime validates complete generated catalog',
            'semantic content failure preserves output', 'runtime object budget enforced', 'object-only pack rejected',
            'last-good ESP and bindings verified together',
            'late first-build cancellation can retry with its existing identity'])
        (workspace/'pack-builder-result.json').write_text(json.dumps(report, indent=2))
        print(json.dumps(report))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--application', required=True)
    parser.add_argument('--workspace', required=True)
    parser.add_argument('--pack', action='append', required=True)
    asyncio.run(run(parser.parse_args()))
