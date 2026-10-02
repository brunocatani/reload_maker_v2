"""The native library refresh retains the selected source and authored state."""
import json
from pathlib import Path
import sys,uuid
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tests'))
from visible_geometry_mcp_test import ready,finish
from server import command

def run():
    work=ROOT/'local/coverage-mcp-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    before=ready();project=Path(before['project']);original=project.read_bytes()
    result=finish(command('pack_coverage',dict(refresh=True)))
    assert result['scope']=='authored-workspace'
    state=command('pack_coverage');after=ready()
    assert state['scanned'] and len(state['projects'])>=12
    for key in ('project','weaponIndex','slots','magazineGrabGrips'):assert after[key]==before[key],key
    assert project.read_bytes()==original
    loaded=state['loadedWeapons'];assert loaded
    for weapon in loaded:
        if any(w['record'].lower()==weapon['record'] for p in state['projects'] for w in p['weapons']):assert 'Authored' in weapon['coverage'] or 'Review' in weapon['coverage']
    (work/'result.json').write_text(json.dumps(state,indent=2));print('PASS native coverage refresh and source/selection/authoring preservation:',work)

if __name__=='__main__':run()
