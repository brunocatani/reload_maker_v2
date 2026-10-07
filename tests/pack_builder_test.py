"""Native packaging checks only; no weapon behavior or golden-capture suite."""
import argparse
import asyncio
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from product_backend import Backends
from product_packs import build, load_pack, read, write, allocate
from product_grouped import deliver, partition, plugin_name

async def run(args):
    workspace=Path(args.workspace).resolve();workspace.mkdir(parents=True,exist_ok=True)
    backends=Backends(Path(args.application),workspace)
    inputs=[]
    for i,source in enumerate(args.pack):
        target=workspace/'inputs'/str(i);shutil.copytree(source,target,dirs_exist_ok=True);inputs.append(target)
    install=workspace/'installation'
    first=await build(inputs,install,backends,initialize=True,archive=workspace/'reloads.zip')
    ledger=read(install/read(install/'current.json')['generation']/'ledger.json')
    ids=lambda data:{(master,key):v['localId'] for master,g in data['groups'].items() for key,v in g['objects'].items()}
    original_ids=ids(ledger)
    assert all(v<=0xfff for v in original_ids.values())
    assert len(first['plugins'])==len({w['record'].split(':')[0].lower() for p in inputs for w in load_pack(p)['manifest']['weapons']})
    with zipfile.ZipFile(workspace/'reloads.zip') as archive:
        assert set(g['plugin'] for g in first['plugins']).issubset(archive.namelist())
        assert not any(n.endswith('DIAMOND_Generated.esp') for n in archive.namelist())
        assert len(archive.namelist())==len([p for p in Path(first['output']).rglob('*') if p.is_file()])
    for group in first['plugins']:
        checked=subprocess.run([str(backends.application/'backends/diamond/DiamondProfileCheck.exe'),
                                '--catalog-enabled',first['output'],group['plugin']],capture_output=True,text=True)
        active=json.loads(checked.stdout)
        assert checked.returncode==0 and active['objects']==group['objects'],active
    second=await build(list(reversed(inputs)),install,backends)
    assert ids(read(install/read(install/'current.json')['generation']/'ledger.json'))==original_ids
    for g in first['plugins']:
        assert (Path(first['output'])/g['plugin']).read_bytes()==(Path(second['output'])/g['plugin']).read_bytes()
    # A named MO2 folder receives one bundle; source plugins/profile files are never written.
    mod=workspace/'mo2'/'mods'/'Reloads';mod.parent.mkdir(parents=True,exist_ok=True)
    deliver(Path(first['output']),mo2=mod)
    marker=mod/'user-owned.txt';marker.write_text('keep')
    subset=await build(inputs[:1],install,backends,mo2=mod)
    assert marker.read_text()=='keep'
    assert ids(read(install/read(install/'current.json')['generation']/'ledger.json'))==original_ids
    assert set(g['plugin'] for g in first['plugins']).issubset({p.name for p in mod.glob('*.esl')})
    assert backends.content_validation(mod)['ok']
    # Editing another source and returning must retain the first source's siblings.
    source_master=load_pack(inputs[0])['manifest']['weapons'][0]['record'].split(':')[0].lower()
    other=[p for p in inputs if load_pack(p)['manifest']['weapons'][0]['record'].split(':')[0].lower()!=source_master]
    if other:
        await build(other,install,backends)
        returned=await build(inputs[:1],install,backends)
        expected=next(g['objects'] for g in first['plugins'] if g['master']==source_master)
        assert returned['activeObjects']==expected, 'Interleaved update lost sibling weapons'
    # A damaged input must not promote an installation or overwrite a completed ZIP.
    pointer=(install/'current.json').read_bytes();previous_zip=(workspace/'reloads.zip').read_bytes()
    pack=load_pack(inputs[0]);obj=inputs[0]/pack['manifest']['objects'];original=obj.read_bytes();obj.write_text('[]')
    try:
        await build(inputs,install,backends,archive=workspace/'reloads.zip')
        raise AssertionError('empty magazine object list accepted')
    except ValueError:
        pass
    finally:
        obj.write_bytes(original)
    assert (install/'current.json').read_bytes()==pointer and (workspace/'reloads.zip').read_bytes()==previous_zip
    # Every source group has its own bounded compact ID space.
    group=copy.deepcopy(next(iter(ledger['groups'].values())));group['nextId']=0x1000
    try:
        allocate(group,[dict(manifest={'id':'new'},objects=[{'id':'new/item'}])])
        raise AssertionError('ESL form-space overflow accepted')
    except ValueError as e:
        assert 'allocation' in str(e)
    result={'ok':True,'plugins':first['plugins'],'objects':first['activeObjects'],
            'checks':['source grouping','native ESL flag and record readback','mesh namespaces','ZIP contents',
                      'stable reordered per-group IDs','additive MO2 delivery','interleaved source updates retain siblings','failed input preserves completed output','compact ID ceiling']}
    write(workspace/'result.json',result);print(json.dumps(result))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--application',required=True);p.add_argument('--workspace',required=True);p.add_argument('--pack',action='append',required=True)
    asyncio.run(run(p.parse_args()))
