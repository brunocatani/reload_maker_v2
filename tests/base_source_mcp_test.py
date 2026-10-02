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
        coverage=command('pack_coverage');loaded=coverage['loadedWeapons'];assert len(loaded)>100 and coverage['sourceMode']=='explicit-data'
        assert all(w['owner'].lower() in ('fallout4.esm','fallout4_vr.esm') for w in loaded)
        definition=json.loads(Path(state['source']).read_text());assert definition['resourceData'] and definition['archives']
        (work/'catalog.json').write_text(json.dumps(coverage,indent=2))
        candidates=[w for w in state['weapons'] if '10mm' in w['editorId'].lower()]
        assert candidates,'Native 10mm source weapon missing'
        selected=min(candidates,key=lambda w:len(w['editorId']))
        edit('select_weapon',dict(index=selected['index']));ready();edit('workspace',dict(index=5));ready()
        before=command('simulation_state');assert 'Choose a behavior template' in before['error'],before
        edit('roadmap',dict(operation='template',id='pistol.slide-detachable.v1'));ready()
        assert command('roadmap')['templateAssigned']
        assert 'Choose a behavior template' not in command('simulation_state')['error']
        edit('roadmap',dict(operation='record-sources'));audit=command('roadmap',dict(operation='check-sources'))
        assert audit['compatible'] and audit['sourceMode']=='explicit-data'
        base=work/'base.project.json';edit('save_project',dict(path=str(base)));edit('open_project',dict(path=str(base)));ready()
        assert command('pack_coverage')['sourceMode']=='explicit-data'
        (work/'result.json').write_text(json.dumps(dict(ok=True,weapons=len(loaded),selected=selected,source=audit,simulation=command('simulation_state')),indent=2))
        print('PASS real base Data import, explicit source audit, source/template resume and previous project preservation:',work,flush=True)
    except Exception:
        (work/'failure.json').write_text(json.dumps(dict(state=command('state'),diagnostics=command('diagnostics')),indent=2));print('Failure evidence:',work,flush=True);raise
    finally:
        Path(original).write_bytes(saved);command('open_project',dict(path=original),command('state')['revision']);ready()
        edit('save_project',dict(path=original));Path(original).write_bytes(saved)

if __name__=='__main__':run()
