"""The library coverage refresh reads saved ESP projects and keeps the selected source and authoring."""
import json
from pathlib import Path
import sys,uuid
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tests'))
from visible_geometry_mcp_test import ready
from server import command

def run():
    work=ROOT/'local/coverage-mcp-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    before=ready()
    state=command('pack_coverage',dict(refresh=True),before['revision'])
    after=ready()
    assert state['scope']=='esp-library' and state['scanned'] and state['projects'],state.get('scope')
    for key in ('project','weaponIndex','slots','magazineGrabGrips'):assert after[key]==before[key],key
    saved={w['record'].lower() for p in state['projects'] for w in p['weapons']}
    loaded=state['loadedWeapons'];assert loaded
    for weapon in loaded:
        expected=('Saved','Unsaved edits') if weapon['record'] in saved else ('Not authored','Unsaved edits')
        assert weapon['coverage'].startswith(expected),weapon
    (work/'result.json').write_text(json.dumps(state,indent=2));print('PASS library coverage refresh and source/selection/authoring preservation:',work)

if __name__=='__main__':run()
