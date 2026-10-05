"""Two weapons sharing an OMOD still need separate world-object hand identities."""
import argparse
import copy
from pathlib import Path
import shutil
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from product_authoring import save_configuration
from product_backend import Backends
from product_export import inspect_project
from product_packs import read,write

def run(capture,assets):
    work=ROOT/'local/multiweapon-pack-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    backend=Backends(ROOT/'build-fast/dist/ReloadMakerV2',ROOT);project=work/'pack.project.json'
    first=save_configuration(project,capture,backend,pack_id='fixture.shared-magazine',
        full=str(assets/'full/finished.nif'),empty=str(assets/'empty/finished.nif'))
    original=read(project)
    second=read(capture);weapon='FixturePistol.esp:000001';second['weapon']=weapon;second['name']='Independent pistol'
    second['profileHeader']['weapon']=weapon;second['profile']['source']['weaponRecord']=weapon
    second['sounds']['weapon']=weapon
    second['magazine']['grip']['placement']['handInAnchor']['translate'][0]+=1
    assert not second['grabs'],'This fixture uses the unassigned Glock policy'
    models={}
    for state in ('full','empty'):
        model=work/(state+'.nif');shutil.copy2(assets/state/'finished.nif',model)
        receipt=read(assets/state/'finished.receipt.json');receipt['capture']['weapon']=weapon
        write(model.with_suffix('.receipt.json'),receipt);models[state]=str(model)
    snapshot=work/'second.json';write(snapshot,second)
    save_configuration(project,snapshot,backend,full=models['full'],empty=models['empty'])
    current,coverage=inspect_project(project);assert len(coverage)==2 and len(current['objects'])==4
    a,b=(w['magazineObjects'][0] for w in current['weapons']);assert a['record']==b['record']
    assert {a['full'],a['empty']}.isdisjoint({b['full'],b['empty']})
    assert current['weapons'][0]==original['weapons'][0]
    invalid=copy.deepcopy(current);invalid['weapons'][1]['magazineObjects'][0]=copy.deepcopy(a)
    write(project,invalid)
    try:inspect_project(project)
    except ValueError:pass
    else:raise AssertionError('Two independent hand fits shared one world record')
    write(project,current)
    print('PASS shared source magazine, independent weapon fits/objects, unchanged first weapon and duplicate-world rejection:',work)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--capture',type=Path,required=True);parser.add_argument('--assets',type=Path,required=True)
    args=parser.parse_args();run(args.capture,args.assets)
