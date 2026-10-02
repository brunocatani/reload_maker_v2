"""Selected-config saves replace identities and preserve unrelated authored work."""
import argparse
import copy
import json
from pathlib import Path
import shutil
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from product_authoring import save_configuration
from product_backend import Backends
from product_export import inspect_project,project_source
from product_packs import digest,read,write
sys.path.insert(0,str(ROOT/'tests'))
from runtime_capture_mcp_test import NAMES

def run(captures,assets):
    work=ROOT/'local/authoring-service-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    backends=Backends(ROOT/'build-fast/dist/ReloadMakerV2',ROOT)
    index=read(ROOT/'workspace/packs/verified-b5bf9bf910a249da8f180b951aa17ae1/projects.json')
    results=[]
    for capture in captures.glob('*/capture.json'):
        name=capture.parent.name;source=Path(next(p['path'] for p in index if p['name']==NAMES[name]))
        project=work/name/'pack.project.json';shutil.copytree(source.parent,project.parent)
        original,coverage=inspect_project(project);old=copy.deepcopy(original);first_hash=digest(project)
        result=save_configuration(project,capture,backends,expected=first_hash)
        saved,after=inspect_project(project);assert coverage==after
        assert saved['objects']==old['objects'] and saved['weapons'][0]['magazineObjects']==old['weapons'][0]['magazineObjects']
        before_docs={k:read(project.parent/v['path']) for k,v in old['weapons'][0].items() if k in ('profiles','magazines','sounds','grabs')}
        new_docs={k:read(project_source(project,saved['weapons'][0][k])) for k in before_docs}
        selected=read(capture)['selection']
        for p in before_docs['profiles']['profiles']:
            if any(p.get(k,before_docs['profiles'].get('standardReceiver'))!=v for k,v in selected.items()):
                assert p in new_docs['profiles']['profiles'],(name,'Unrelated motion changed')
        again=save_configuration(project,capture,backends,expected=result['sha256'])
        repeated,_=inspect_project(project)
        for kind in new_docs:assert new_docs[kind]==read(project_source(project,repeated['weapons'][0][kind]))
        try:save_configuration(project,capture,backends,expected=first_hash)
        except ValueError:pass
        else:raise AssertionError('Stale writer accepted')
        assert digest(project)==again['sha256']
        presentation=read(capture);presentation['magazine']['presentationSetup']={'recipeId':presentation['magazine']['sourceRecipeId'],'pieces':[{'path':'changed','role':'cartridge'}]}
        changed=work/name/'presentation.json';write(changed,presentation)
        try:save_configuration(project,changed,backends,expected=again['sha256'])
        except ValueError as error:assert 'recapture and cook' in str(error)
        else:raise AssertionError('Changed payload reused old cooked models')
        assert digest(project)==again['sha256']
        bad=read(capture);bad['magazine']['grip']['fingerLocalTransforms']=[]
        write(work/name/'invalid.json',bad)
        try:save_configuration(project,work/name/'invalid.json',backends,expected=again['sha256'])
        except ValueError:pass
        else:raise AssertionError('Malformed hand accepted')
        assert digest(project)==again['sha256']
        if assets and (assets/name/'empty/finished.nif').exists():
            new=work/name/'new/pack.project.json'
            created=save_configuration(new,capture,backends,pack_id='authored.'+name,
                full=str(assets/name/'full/finished.nif'),empty=str(assets/name/'empty/finished.nif'))
            _,one=inspect_project(new);assert len(one)==1 and one[0]['variants']==1 and one[0]['magazines']==1
        results.append(dict(testbed=name,variants=coverage[0]['variants']));print('PASS',name,flush=True)
    write(work/'result.json',dict(ok=True,results=results));print('Evidence:',work)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--captures',type=Path,required=True);parser.add_argument('--assets',type=Path)
    args=parser.parse_args();run(args.captures,args.assets)
