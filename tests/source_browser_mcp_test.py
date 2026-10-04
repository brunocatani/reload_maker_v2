"""Exercise the shared MENU source/plugin/weapon flow in the real editor."""
import json
from pathlib import Path
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp'))
from server import command

def ready():
    until=time.monotonic()+180
    while time.monotonic()<until:
        try:
            state=command('state')
            if not state['busy']:
                assert not state['lastError'],state['lastError']
                return state
        except FileNotFoundError:pass
        time.sleep(.2)
    raise AssertionError('Source load did not finish')

def edit(**values):
    command('source_browser',values,ready()['revision'])
    ready()
    return command('source_browser')

def run():
    ready();work=ROOT/'local/source-browser';work.mkdir(parents=True,exist_ok=True)
    observations=[]
    def save(label):
        value=command('source_browser');observations.append(dict(label=label,state=value))
        (work/'interaction.json').write_text(json.dumps(observations,indent=2),encoding='utf-8');return value
    try:
        initial=save('initial dynamic')
        assert initial['mode']=='dynamic',initial['mode']
        if not initial['plugin']:assert not initial['weapons'],'Unselected source exposed weapons'
        names=[p['name'] for p in initial['plugins'] if p['name']!='*']
        install=command('installation');profile=Path(install['profilePath'])
        enabled={s[1:].strip().lower() for s in (profile/'plugins.txt').read_text(encoding='utf-8-sig').splitlines() if s.startswith('*')}
        enabled.update(('fallout4.esm','fallout4_vr.esm'))
        order=[s.strip() for s in (profile/'loadorder.txt').read_text(encoding='utf-8-sig').splitlines() if s.strip().lower() in enabled]
        assert [s.lower() for s in names]==[s.lower() for s in order],'Plugin list does not match MO2 load order'
        everything=edit(operation='plugin',plugin='*')
        assert len(everything['weapons'])==len(ready()['weapons']) and everything['weapons']
        assert len({w['record'].split(':')[0].lower() for w in everything['weapons']})>1
        assert all(w['record'].split(':')[0].lower() in w['name'].lower() for w in everything['weapons']),'All-weapons rows lost their source ESP'
        weapon=next(w for w in everything['weapons'] if w['record'].lower()=='glock19x.esp:000f9b')
        selected=edit(operation='weapon',index=weapon['index']);assert selected['plugin']=='*','Choosing a weapon left all-weapons mode'
        fixture=work/'all-weapons.project.json'
        command('save_project',{'path':str(fixture)},ready()['revision'])
        command('open_project',{'path':str(fixture)},ready()['revision']);ready()
        assert command('source_browser')['plugin']=='*' and ready()['weaponIndex']==weapon['index'],'Reopening lost the all-weapons selection'
        save('all weapons across ESPs and project reopen')
        glock=edit(operation='plugin',plugin='Glock19x.esp')
        assert glock['weapons'] and all(w['record'].lower().startswith('glock19x.esp:') for w in glock['weapons'])
        assert all('glock19x.esp' not in w['name'].lower() for w in glock['weapons']),'Scoped picker retained all-weapons ESP suffixes'
        assert next(p for p in glock['plugins'] if p['name'].lower()=='fallout4.esm')['role']=='master'
        empty=edit(operation='plugin',plugin='DLCworkshop01.esm');assert not empty['weapons'],'Empty plugin exposed all weapons'
        dev=edit(operation='mode',mode='development');save('development Glock')
        assert dev['mode']=='development' and dev['plugin'].lower()=='glock19x.esp'
        assert all(p['name']!='*' for p in dev['plugins']),'All-weapons option leaked into development mode'
        assert next(p for p in dev['plugins'] if p['name'].lower()=='fallout4.esm')['role']=='master'
        master=edit(operation='plugin',plugin='Fallout4.esm')
        assert master['weapons'] and all(w['record'].lower().startswith('fallout4.esm:') for w in master['weapons'])
        assert any(w['name'].startswith('Combat Knife') for w in master['weapons']),'Development master melee missing'
        rifle=edit(operation='plugin',plugin='L118A2.esp');save('development shared ESP')
        assert any(w['record'].lower()=='l118a2.esp:00206c' for w in rifle['weapons'])
        other=next(w for w in rifle['weapons'] if w['record'].lower()=='l118a2.esp:000002')
        edit(operation='weapon',index=other['index']);state=ready()
        assert state['weaponIndex']>=0 and state['weapons'][state['weaponIndex']]['editorId']=='L118A2'
        assert command('simulation_state')['active'],'Saved L118A2 source did not restore its operating setup'
        save('development L118A2 configured source')
        edit(operation='mode',mode='dynamic');edit(operation='plugin',plugin='Glock19x.esp');save('restored dynamic')
        print('PASS MENU source flow, MO2 order, strict plugin filtering, development masters and saved testbed switching',flush=True)
    except Exception:
        (work/'failure.json').write_text(json.dumps(dict(state=command('state'),browser=command('source_browser'),diagnostics=command('diagnostics')),indent=2),encoding='utf-8')
        raise

if __name__=='__main__':run()
