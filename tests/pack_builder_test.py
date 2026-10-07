"""Native export checks on copies of saved ESP projects; no weapon behavior or
golden-capture suite.

Usage: pack_builder_test.py --application DIST --workspace SCRATCH --plugin Glock19x.esp [--plugin ...]
"""
import argparse
import asyncio
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import product_library as library
from product_backend import Backends
from product_grouped import export_library
from product_packs import read, write, allocate


def identities(workspace,plugins):
    return {p:read(library.project_folder(workspace,p)/'export/identity.json') for p in plugins}


async def run(args):
    workspace=Path(args.workspace).resolve();workspace.mkdir(parents=True,exist_ok=True)
    for plugin in args.plugin:
        target=library.project_folder(workspace,plugin);shutil.rmtree(target,ignore_errors=True)
        shutil.copytree(library.project_folder(ROOT,plugin),target)
    backends=Backends(Path(args.application),workspace)
    selections=[dict(plugin=p) for p in args.plugin]
    archive=workspace/'reloads.zip';archive.unlink(missing_ok=True)
    first=await export_library(workspace,selections,backends,archive=archive)
    assert all(g['plugin'].endswith('.esp') for g in first['plugins']) and len(first['plugins'])==len(args.plugin),first['plugins']
    with zipfile.ZipFile(archive) as bundle:
        names=bundle.namelist()
        assert {g['plugin'] for g in first['plugins']}<=set(names) and not any(n.endswith('DIAMOND_Generated.esp') for n in names)
    ids=identities(workspace,args.plugin)
    assert all(e['localId']<=0xfff for value in ids.values() for e in value['group']['objects'].values())
    # A named MO2 folder receives the bundle; repeating it in another order
    # changes no ID and rewrites no file.
    mod=workspace/'mo2'/'mods'/'Reloads';shutil.rmtree(mod,ignore_errors=True);mod.mkdir(parents=True)
    marker=mod/'user-owned.txt';marker.write_text('keep')
    installed=await export_library(workspace,selections,backends,mo2=mod)
    for group in installed['plugins']:
        checked=subprocess.run([str(backends.application/'backends/diamond/DiamondProfileCheck.exe'),'--catalog-enabled',str(mod),group['plugin']],capture_output=True,text=True)
        active=json.loads(checked.stdout)
        assert checked.returncode==0 and active['objects']==group['objects'],active
    again=await export_library(workspace,list(reversed(selections)),backends,mo2=mod)
    assert again['written']==0 and again['unchanged']==installed['written']+installed['unchanged'],again
    assert identities(workspace,args.plugin)==ids,'A repeated export changed object IDs'
    assert marker.read_text()=='keep' and backends.content_validation(mod)['ok']
    # Exporting one ESP leaves every other ESP's IDs alone.
    if len(args.plugin)>1:
        await export_library(workspace,selections[:1],backends,archive=workspace/'one.zip')
        assert identities(workspace,args.plugin)==ids
    # A damaged saved version cannot replace a completed archive or change IDs.
    folder=library.project_folder(workspace,args.plugin[0]);project=library.read_project(folder)
    snapshot=library.read_snapshot(folder,project['latest'])
    entry=next(e for e in snapshot['weapons'].values() if e.get('pack'))
    record=folder/entry['pack'];original=record.read_bytes();damaged=json.loads(original);damaged['objects']=[]
    previous=archive.read_bytes();record.write_text(json.dumps(damaged))
    weapons=sum(1 for e in snapshot['weapons'].values() if e.get('pack'))
    try:
        if weapons==1:
            try:
                await export_library(workspace,selections[:1],backends,archive=archive)
                raise AssertionError('a version with only a damaged pack was exported')
            except ValueError:
                pass
            assert archive.read_bytes()==previous and identities(workspace,args.plugin)==ids
        else:
            # The other weapons still export; the damaged one is reported.
            result=await export_library(workspace,selections[:1],backends,archive=workspace/'partial.zip')
            assert any(s['name']==entry.get('name',s['name']) for s in result['skipped']),result['skipped']
    finally:
        record.write_bytes(original)
    # Every source group has its own bounded compact ID space.
    group=copy.deepcopy(ids[args.plugin[0]]['group']);group['nextId']=0x1000
    try:
        allocate(group,[dict(manifest={'id':'new'},objects=[{'id':'new/item'}])])
        raise AssertionError('ESL form-space overflow accepted')
    except ValueError as e:
        assert 'allocation' in str(e)
    result={'ok':True,'plugins':first['plugins'],'objects':first['activeObjects'],
            'checks':['one ESL-flagged ESP per source','native catalog readback','ZIP contents','stable reordered IDs',
                      'identical files left in place','other ESP IDs untouched','failed input preserves completed output','compact ID ceiling']}
    write(workspace/'result.json',result);print(json.dumps(result))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--application',required=True);p.add_argument('--workspace',required=True);p.add_argument('--plugin',action='append',required=True)
    asyncio.run(run(p.parse_args()))
