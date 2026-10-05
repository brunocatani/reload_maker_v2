"""Real base Data import preserves the previous document and requires a template."""
import json,sys,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit
from server import command

def run():
    work=ROOT/'local/base-source-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=ready()['project'];saved=Path(original).read_bytes()
    try:
        assert not command('installation')['virtualFilesystem'],'Use a normal editor process for the explicit Data test'
        edit('save_project',dict(path=str(work/'before.project.json')))
        edit('base_game',{});state=ready();assert not state['project'],'Base import retained the old save destination'
        coverage=command('pack_coverage');loaded=coverage['loadedWeapons'];assert coverage['sourceMode']=='explicit-data',coverage['sourceMode']
        # Non-playable records (companion, creature and dummy weapons) are
        # hidden with a reason; the base game lists 88 playable weapons.
        excluded=coverage['weaponFilter']['excluded'];assert len(loaded)>=80 and excluded,(len(loaded),len(excluded))
        assert all(x['reason'] for x in excluded) and not {x['record'] for x in excluded}&{w['record'] for w in loaded}
        assert all(w['owner'].lower() in ('fallout4.esm','fallout4_vr.esm') for w in loaded)
        definition=json.loads(Path(state['source']).read_text());assert definition['resourceData'] and definition['archives']
        (work/'catalog.json').write_text(json.dumps(coverage,indent=2))
        candidates=[w for w in state['weapons'] if '10mm' in w['editorId'].lower()]
        assert candidates,'Native 10mm source weapon missing'
        selected=min(candidates,key=lambda w:len(w['editorId']))
        edit('select_weapon',dict(index=selected['index']));ready();edit('workspace',dict(index=5));ready()
        before=command('simulation_state');assert "Confirm this weapon's behavior preset" in before['error'],before
        edit('checklist',dict(operation='preset',id='pistol.slide-detachable.v1'));ready()
        assert command('checklist_state')['presetAssigned']
        assert 'behavior preset' not in command('simulation_state')['error']
        edit('checklist',dict(operation='confirm',id='weapon.sources'))
        base=work/'base.project.json';edit('save_project',dict(path=str(base)))
        evidence=next(v['sources'] for v in json.loads(base.read_text(encoding='utf-8'))['authoringRoadmaps'].values() if 'sources' in v)
        audit=command('inspect_sources',dict(evidence=evidence))
        assert audit['compatible'] and audit['sourceMode']=='explicit-data'
        edit('open_project',dict(path=str(base)));ready()
        assert command('pack_coverage')['sourceMode']=='explicit-data'
        (work/'result.json').write_text(json.dumps(dict(ok=True,weapons=len(loaded),selected=selected,source=audit,simulation=command('simulation_state')),indent=2))
        print('PASS real base Data import, explicit source audit, source/template resume and previous project preservation:',work,flush=True)
    except Exception:
        (work/'failure.json').write_text(json.dumps(dict(state=command('state'),diagnostics=command('diagnostics')),indent=2));print('Failure evidence:',work,flush=True);raise
    finally:
        Path(original).write_bytes(saved);command('open_project',dict(path=original),command('revision')['revision']);ready()
        edit('save_project',dict(path=original));Path(original).write_bytes(saved)

if __name__=='__main__':run()
