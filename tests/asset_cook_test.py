"""Native prepared-magazine cook and failure preservation, using product tools."""
import argparse
import json
import os
from pathlib import Path
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from product_assets import cook_world_object
from product_backend import Backends
from product_packs import digest,read,write


def run(args):
    work=ROOT/'local/asset-cook-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    backend=Backends(ROOT/'build-fast/dist/ReloadMakerV2',ROOT)
    job=work/'job.json'
    value=dict(format='reload-maker-world-object-job',version=1,
        geometry=dict(path=str(args.model.resolve()),sha256=digest(args.model)),
        collision=dict(path=str(args.hulls.resolve()),sha256=digest(args.hulls)))
    write(job,value)
    result=cook_world_object(job,work/'magazine.nif',backend)
    receipt=read(Path(result['receipt']))
    assert receipt['engineCooked'] and not receipt['gameTested'] and receipt['hulls']>0
    output_hash=digest(Path(result['output']))
    try:cook_world_object(job,Path(result['output']),backend)
    except ValueError as error:assert 'new finished' in str(error)
    else:raise AssertionError('Prior finished object was overwritten')
    assert digest(Path(result['output']))==output_hash
    cancelled=work/'cancel';cancelled.touch();os.environ['RELOAD_MAKER_CANCEL_FILE']=str(cancelled)
    try:
        try:cook_world_object(job,work/'cancelled.nif',backend)
        except RuntimeError as error:assert 'cancel' in str(error).lower()
        else:raise AssertionError('Cancelled cook completed')
        assert not (work/'cancelled.nif').exists()
    finally:os.environ.pop('RELOAD_MAKER_CANCEL_FILE',None)
    value['geometry']['sha256']='0'*64;write(job,value)
    try:cook_world_object(job,work/'stale.nif',backend)
    except ValueError as error:assert 'changed' in str(error)
    else:raise AssertionError('Changed geometry was cooked')
    assert not (work/'stale.nif').exists()
    write(work/'result.json',dict(ok=True,result=result,checks=['native physics','rigid geometry and material preservation','immutable previous output','cancellation','stale input rejection']))
    print('PASS native magazine cooking and output preservation:',work)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--model',type=Path,required=True);parser.add_argument('--hulls',type=Path,required=True)
    run(parser.parse_args())
