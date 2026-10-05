"""A user-selected cartridge exclusion reaches cooked models and the runtime pack."""
import json
from pathlib import Path
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'tools')]
from visible_geometry_mcp_test import ready,edit,finish,reference
from server import command
from product_authoring import save_configuration
from product_backend import Backends
from product_export import inspect_project,project_source
from product_packs import read,write

def run():
    work=ROOT/'local/magazine-piece-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=ready()['project'];original_bytes=Path(original).read_bytes()
    try:
        edit('testbed',dict(name='glock'));ready();project=Path(ready()['project']);saved=project.read_bytes()
        try:
            edit('workspace',dict(index=5));ready();settings=command('magazine_setup_state')['settings']
            edit('capture_magazine_world',dict(empty=False));before=ready('collision_state');count=len(before['sources'])
            edit('workspace',dict(index=5));ready()
            cartridge=next(p for p in settings['pieces'] if p['role']=='cartridge');cartridge['role']='spare'
            edit('magazine_setup',dict(operation='save',settings=settings));ready();models={}
            for empty in (False,True):
                label='empty' if empty else 'full';folder=work/label;folder.mkdir()
                edit('workspace',dict(index=5));ready();edit('capture_magazine_world',dict(empty=empty));state=ready('collision_state')
                if not empty:assert len(state['sources'])==count-1,'Excluded cartridge still in world geometry'
                finish(edit('capture_prepared_part',dict(output=str(folder/'prepared.nif'))))
                hulls=folder/'hulls.json';edit('collisions',dict(operation='export-hulls',path=str(hulls)))
                job=folder/'job.json';write(job,dict(format='reload-maker-world-object-job',version=1,geometry=reference(folder/'prepared.nif'),collision=reference(hulls)))
                result=finish(edit('pack_job',dict(operation='cook',job=str(job),output=str(folder/'finished.nif'))))
                receipt=read(Path(result['receipt']));assert receipt['capture']['referencePose']['presentationSetup']==settings
                models[label]=str(folder/'finished.nif')
            edit('workspace',dict(index=5));ready();capture=Path(edit('export',dict(kind='runtime_configuration'))['path']);record=read(capture)
            assert len(record['magazine']['excludedFullPieces'])==1
            backend=Backends(ROOT/'build-fast/dist/ReloadMakerV2',ROOT);pack=work/'pack.project.json'
            save_configuration(pack,capture,backend,pack_id='fixture.magazine-pieces',**models)
            value,_=inspect_project(pack);document=read(project_source(pack,value['weapons'][0]['magazines']))
            assert document['magazines'][0]['excludedFullPieces']==record['magazine']['excludedFullPieces']
            image=command('render',dict(view='iso',fit=True));write(work/'image.json',image)
            write(work/'result.json',dict(ok=True,sourceFullPieces=count,configuredFullPieces=count-1,models=models,pack=str(pack)))
            print('PASS piece exclusion, both cooked states, matched receipts and native runtime validation:',work,flush=True)
        finally:
            project.write_bytes(saved);command('open_project',dict(path=str(project)),command('revision')['revision']);ready()
            edit('save_project',dict(path=str(project)));project.write_bytes(saved)
    finally:
        Path(original).write_bytes(original_bytes);command('open_project',dict(path=original),command('revision')['revision']);ready()
        edit('save_project',dict(path=original));Path(original).write_bytes(original_bytes)

if __name__=='__main__':run()
