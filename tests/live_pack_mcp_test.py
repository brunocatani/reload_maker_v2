"""One manual-equivalent live capture -> cooked models -> Save -> exported ESP."""
import json
import time
from pathlib import Path
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from visible_geometry_mcp_test import ready,edit,finish,reference
from server import command

def read(path):return json.loads(path.read_text())
def wait(job,timeout=1800):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        state=command('pack_job')
        if state.get('id')==job['id'] and state.get('phase') not in ('running','cancelling'):
            assert state['phase']=='complete',state
            return state['result']
        time.sleep(.2)
    raise AssertionError('Save or export timed out')


def run():
    work=ROOT/'local/live-pack-mcp'/uuid.uuid4().hex;work.mkdir(parents=True)
    edit('testbed',{'name':'glock'});ready();plugin=command('esp_project')['plugin'];created=[]
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
        # The finished models are authoring of this magazine; Save commits them.
        assigned=edit('assign_magazine_models',dict(full=models['full'],empty=models['empty']))
        assert assigned.get('assigned',True) and ready()['modified'] is True
        saved=wait(edit('save_project',dict(note='live pack')));created.append(saved['save']['id'])
        weapon=next(w for w in saved['save']['weapons'] if w['exportable'])
        assert weapon['magazines']>=1,weapon
        archive=work/'export.zip'
        exported=wait(edit('pack_job',dict(operation='export',selections=[dict(plugin=plugin,save=saved['save']['id'])],archive=str(archive))))
        assert archive.is_file() and exported['activeObjects']>=2,exported
        # Swapping an empty hull set under the full geometry cannot cook.
        wrong=read(work/'full/job.json');wrong['collision']=reference(work/'empty/hulls.json')
        (work/'wrong.json').write_text(json.dumps(wrong))
        try:finish(edit('pack_job',dict(operation='cook',job=str(work/'wrong.json'),output=str(work/'wrong.nif'))))
        except AssertionError:pass
        else:raise AssertionError('Mismatched full geometry / empty collision accepted')
        assert not (work/'wrong.nif').exists()
        (work/'result.json').write_text(json.dumps(dict(ok=True,saved=saved,exported=exported,screens=screens),indent=2))
        print('PASS live configuration, full/empty carrier models, Save into the ESP project, archive export and mismatched-state rejection:',work,flush=True)
    finally:
        for save_id in created:
            try:edit('esp_version',dict(operation='delete',save=save_id));ready()
            except RuntimeError as error:print('cleanup',plugin,save_id,error,flush=True)

if __name__=='__main__':run()
