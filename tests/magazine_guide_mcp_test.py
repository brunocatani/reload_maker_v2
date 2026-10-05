"""Straight input guide edits preserve source visual motion and saved hand fits."""
import argparse
import json
from pathlib import Path
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit,simulation
from server import command

def run(names):
    work=ROOT/'local/magazine-guide-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=ready()['project'];original_bytes=Path(original).read_bytes();results=[]
    try:
        for name in names:
            edit('testbed',dict(name=name));ready();project=Path(ready()['project']);saved=project.read_bytes()
            try:
                edit('workspace',dict(index=5));ready();before=simulation();state=command('magazine_guide_state');settings=state['settings']
                # A recipe id is provenance, never a gate (owner, 2026-10-05).
                edit('magazine_guide',dict(operation='save',settings=dict(settings,recipeId='0'*64)));ready()
                assert command('magazine_guide_state')['settings']['recipeId']!='0'*64,'A foreign recipe id was kept'
                revised=dict(settings,outwardInConnection=[.6,0,-.8],travel=settings['travel']*1.25)
                edit('magazine_guide',dict(operation='save',settings=revised));ready();after=simulation()
                a,b=before.pop('magazineInteraction'),after.pop('magazineInteraction')
                assert before==after,(name,'Guide changed other simulation/hand data')
                assert b['inputSetup']==command('magazine_guide_state')['settings']
                assert abs(b['travel']-revised['travel'])<.0001
                if 'motionSamples' in a:assert a['motionSamples']==b['motionSamples'],(name,'Guide modified the full visible curve')
                else:
                    assert len(b['motionSamples'])==2
                    end=b['motionSamples'][-1]['deltaInWeapon']['translate']
                    assert max(abs(x-a['travel']*y) for x,y in zip(end,a['outwardInWeapon']))<.001
                edit('simulate',dict(operation='mag_out'));edit('simulate',dict(operation='step',seconds=1))
                capture=command('render',dict(view='iso',fit=True))
                (work/(name+'-image.json')).write_text(json.dumps(capture,indent=2))
                edit('simulate',dict(operation='reset'))
                edit('save_project',dict(path=str(work/(name+'.project.json'))))
                edit('open_project',dict(path=str(work/(name+'.project.json'))));ready()
                assert simulation()['magazineInteraction']==b
                edit('magazine_guide',dict(operation='reset'));ready();restored=simulation();assert restored.pop('magazineInteraction')==a and restored==before
                results.append(dict(testbed=name,visualPreserved=True,reopen=True,reset=True));print('PASS',name,flush=True)
            finally:
                project.write_bytes(saved);command('open_project',dict(path=str(project)),command('revision')['revision']);ready()
                edit('save_project',dict(path=str(project)));project.write_bytes(saved)
        (work/'result.json').write_text(json.dumps(dict(ok=True,results=results),indent=2));print('Evidence:',work,flush=True)
    finally:
        Path(original).write_bytes(original_bytes);command('open_project',dict(path=original),command('revision')['revision']);ready()
        edit('save_project',dict(path=original));Path(original).write_bytes(original_bytes)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--testbed',action='append')
    run(parser.parse_args().testbed or ['glock','m1a'])
