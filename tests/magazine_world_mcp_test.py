"""Cook full/empty states from actual Operate surfaces in their carrier frame."""
import argparse
import json
from pathlib import Path
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from visible_geometry_mcp_test import ready,edit,finish,reference
from server import command

def run(names):
    work=ROOT/'local/magazine-world-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=ready()['project'];original_bytes=Path(original).read_bytes();results=[]
    try:
        for name in names:
            edit('testbed',{'name':name});ready();project=Path(ready()['project']);saved=project.read_bytes()
            try:
                for empty in (False,True):
                    folder=work/name/('empty' if empty else 'full');folder.mkdir(parents=True)
                    edit('workspace',{'index':5});ready()
                    edit('capture_magazine_world',{'empty':empty});state=ready('collision_state')
                    assert state['target'].endswith('|'+folder.name),state
                    if name in ('timberwolf','l118a2') and not empty:
                        # These source mods pack overlapping projectile options
                        # together. Exercise the user's explicit FMJ selection.
                        for i,source in enumerate(state['sources']):
                            if Path(source['material'].replace('\\','/')).name.lower() in ('338copperprojectileinc.bgsm','338brassprojectile.bgsm','338armorprojectile.bgsm'):
                                edit('collisions',dict(operation='piece',index=i,included=False))
                    prepared=finish(edit('capture_prepared_part',{'output':str(folder/'prepared.nif')}))
                    hulls=folder/'hulls.json';edit('collisions',{'operation':'export-hulls','path':str(hulls)})
                    job=folder/'job.json';job.write_text(json.dumps(dict(format='reload-maker-world-object-job',version=1,
                        geometry=reference(folder/'prepared.nif'),collision=reference(hulls))))
                    result=finish(edit('pack_job',dict(operation='cook',job=str(job),output=str(folder/'finished.nif'))))
                    receipt=json.loads(Path(result['receipt']).read_text());capture=receipt['capture'];frame=capture['referencePose']
                    assert frame['space']=='magazine-carrier' and frame['presentation']==folder.name
                    assert len(receipt['bounds'])==6 and receipt['engineCooked']
                    assert capture==json.loads(hulls.read_text())['capture']
                    results.append(dict(testbed=name,presentation=folder.name,result=result,shapes=prepared['shapes']))
                    print('PASS',name,folder.name,prepared['shapes'],'shapes',result['hulls'],'hulls',flush=True)
            finally:
                project.write_bytes(saved);command('open_project',{'path':str(project)},command('state')['revision']);ready()
        (work/'result.json').write_text(json.dumps(dict(ok=True,results=results),indent=2));print('Evidence:',work,flush=True)
    finally:
        Path(original).write_bytes(original_bytes);command('open_project',{'path':original},command('state')['revision']);ready()

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--testbed',action='append')
    run(parser.parse_args().testbed or ['glock','ru556','m1a','timberwolf','l118a2','thanatos','srep-service','srep-ar10','srep-honeybadger','srep-xm16e1'])
