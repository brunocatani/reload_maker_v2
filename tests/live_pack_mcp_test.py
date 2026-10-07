"""One manual-equivalent live capture -> assets -> authored pack -> local ESP."""
import json
from pathlib import Path
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from visible_geometry_mcp_test import ready,edit,finish,reference
from server import command

def read(path):return json.loads(path.read_text())
def run():
    work=ROOT/'local/live-pack-mcp'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=ready()['project'];original_bytes=Path(original).read_bytes()
    try:
        edit('testbed',{'name':'glock'});ready();canonical=Path(ready()['project']);saved=canonical.read_bytes()
        try:
            models={};screens={}
            for empty in (False,True):
                state='empty' if empty else 'full';folder=work/state;folder.mkdir()
                edit('workspace',{'index':5});ready();edit('capture_magazine_world',{'empty':empty});ready('collision_state')
                simulation=command('simulation_state');assert simulation['active']
                screens[state]=command('render',dict(view='iso',fit=True))['path']
                finish(edit('capture_prepared_part',{'output':str(folder/'prepared.nif')}))
                hulls=folder/'hulls.json';edit('collisions',dict(operation='export-hulls',path=str(hulls)))
                job=folder/'job.json';job.write_text(json.dumps(dict(format='reload-maker-world-object-job',version=1,
                    geometry=reference(folder/'prepared.nif'),collision=reference(hulls))))
                models[state]=finish(edit('pack_job',dict(operation='cook',job=str(job),output=str(folder/'finished.nif'))))['output']
            edit('workspace',{'index':5});ready()
            project=work/'Glock19x.esp/pack.project.json'
            saved_pack=finish(edit('save_configured_reload',dict(project=str(project),id='live.glock',full=models['full'],empty=models['empty'])))
            assert saved_pack['variants']==1 and saved_pack['magazines']==1 and saved_pack['objects']==2
            inspected=finish(edit('pack_job',dict(operation='inspect-project',project=str(project))))
            assert inspected['revision']==saved_pack['revision']
            replaced=finish(edit('save_configured_reload',dict(project=str(project))))
            assert replaced['variants']==1 and replaced['magazines']==1 and replaced['objects']==2
            exported=finish(edit('pack_job',dict(operation='freeze',project=str(project),output=str(work/'pack'))))
            built=finish(edit('pack_job',dict(operation='build',packs=[exported['output']],installation=str(work/'installation'),initialize=True)))
            assert built['activeObjects']==2
            # Swapping an empty hull set under the full geometry cannot cook.
            wrong=read(work/'full/job.json');wrong['collision']=reference(work/'empty/hulls.json')
            (work/'wrong.json').write_text(json.dumps(wrong))
            try:finish(edit('pack_job',dict(operation='cook',job=str(work/'wrong.json'),output=str(work/'wrong.nif'))))
            except AssertionError:pass
            else:raise AssertionError('Mismatched full geometry / empty collision accepted')
            assert not (work/'wrong.nif').exists()
            (work/'result.json').write_text(json.dumps(dict(ok=True,saved=replaced,exported=exported,built=built,screens=screens),indent=2))
            print('PASS live configuration, full/empty carrier models, replacement save, publication, local ESP and mismatched-state rejection:',work,flush=True)
        finally:
            canonical.write_bytes(saved);command('open_project',{'path':str(canonical)},command('revision')['revision']);ready()
    finally:
        Path(original).write_bytes(original_bytes);command('open_project',{'path':original},command('revision')['revision']);ready()

if __name__=='__main__':run()
