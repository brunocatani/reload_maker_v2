"""A real shared-receiver source scopes magazine authoring to its native branch."""
import argparse
import json
from pathlib import Path
import shutil
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit
from server import command


def run(fixture):
    work=ROOT/'local/shared-magazine-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=ready()['project']
    source=work/'source.project.json';shutil.copy2(fixture,source)
    try:
        edit('open_project',dict(path=str(source)));ready();edit('workspace',dict(index=5));ready()
        settings=command('operating_setup_state')['settings']
        settings.update(mode='reload',operating=0,bolt=0,magazine=4,followers=[0,1,2,3],openFrame=-1,closedFrame=-1,fireMode='native')
        edit('operating_setup',dict(operation='save',settings=settings));ready()
        state=command('simulation_state');(work/'initial.json').write_text(json.dumps(state,indent=2),encoding='utf-8')
        assert state['active'],state.get('error')
        reference=command('magazine_reference_state')
        anchors=[a for a in reference['anchors'] if a['name']=='WeaponMagazine']
        assert len(anchors)==1,reference
        value=reference['settings'];value.update(anchorKey=anchors[0]['key'],connectionMode='carrier')
        edit('magazine_reference',dict(operation='save',settings=value));ready()
        presentation=command('magazine_setup_state')
        assert presentation['pieces'] and all(p['track'] in range(4,10) for p in presentation['pieces']),presentation
        state=command('simulation_state')
        assert len(state['magazineRoots'])==1,state['magazineRoots']
        assert not state.get('magazineError'),state.get('magazineError')
        assert not state['magazine']['error'],state['magazine']['error']
        receiver={p['name']:p['world'] for p in state['parts'] if not p['magazineOwner']}
        edit('simulate',dict(operation='drop_magazine'))
        dropped=command('simulation_state');assert dropped['magazineRoots'][0]['flags']&1
        assert receiver=={p['name']:p['world'] for p in dropped['parts'] if not p['magazineOwner']}
        edit('simulate',dict(operation='grab_magazine'))
        edit('simulate',dict(operation='step',seconds=0))
        held=command('simulation_state');assert not held['magazineRoots'][0]['flags']&1
        assert receiver=={p['name']:p['world'] for p in held['parts'] if not p['magazineOwner']}
        image=command('render',dict(view='iso',fit=True,zoom=1.4,preserveCamera=True))
        shutil.copy2(image['path'],work/'detached.png')
        edit('simulate',dict(operation='reset'))
        saved=work/'configured.project.json';edit('save_project',dict(path=str(saved)))
        edit('open_project',dict(path=str(saved)));ready();edit('workspace',dict(index=5));ready()
        assert command('magazine_setup_state')['pieces']==presentation['pieces']
        (work/'result.json').write_text(json.dumps(dict(ok=True,reference=reference,presentation=presentation,simulation=state),indent=2),encoding='utf-8')
        print('PASS real shared-receiver magazine carrier, reference, presentation and project resume:',work,flush=True)
    except Exception:
        (work/'failure.json').write_text(json.dumps(dict(simulation=command('simulation_state'),setup=command('operating_setup_state')),indent=2),encoding='utf-8')
        print('Failure evidence:',work,flush=True);raise
    finally:
        command('open_project',dict(path=original),command('state')['revision']);ready()


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--project',required=True)
    run(parser.parse_args().project)
