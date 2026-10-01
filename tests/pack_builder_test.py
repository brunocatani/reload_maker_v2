"""Stable IDs, selected-pack output and failed-build preservation using native backends."""
import argparse
import asyncio
import copy
import json
from pathlib import Path
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from product_backend import Backends
from product_packs import allocate, build, digest, load_pack, PLUGIN, read


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
        pointer = read(install/'current.json')
        ledger = read(install/pointer['generation']/'ledger.json')
        ids = {k: v['localId'] for k, v in ledger['objects'].items()}
        second = await build(list(reversed(packs)), install, backends)
        reordered = read(install/read(install/'current.json')['generation']/'ledger.json')
        assert ids == {k: v['localId'] for k, v in reordered['objects'].items()}
        assert first['generation']['pluginSha256'] == second['generation']['pluginSha256']
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
        objects = packs[0]/read(packs[0]/'manifest.json')['objects']
        original = objects.read_bytes()
        objects.write_bytes(b'[]')
        try:
            await build(packs, install, backends)
            raise AssertionError('Changed frozen input was accepted')
        except ValueError as exc:
            assert 'changed after export' in str(exc), str(exc)
        assert (install/'current.json').read_bytes() == original_pointer
        objects.write_bytes(original)
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
        restored = await build(packs, install, backends)
        assert restored['activeObjects'] == len(ids)
        report = dict(ok=True, objects=len(ids), checks=['native cooked NIF validation', 'ESP independent readback',
            'stable reordered IDs and plugin bytes', 'removed packs emit no records or models', 'tamper failure preserves output',
            'corrupt ledger rejected', 'reinstall recovers existing IDs'])
        (workspace/'pack-builder-result.json').write_text(json.dumps(report, indent=2))
        print(json.dumps(report))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--application', required=True)
    parser.add_argument('--workspace', required=True)
    parser.add_argument('--pack', action='append', required=True)
    asyncio.run(run(parser.parse_args()))
