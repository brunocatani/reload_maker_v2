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
        assert initial['mode']=='dynamic' and not initial['plugin'] and not initial['weapons'],initial
        names=[p['name'] for p in initial['plugins']]
        install=command('installation');profile=Path(install['profilePath'])
        enabled={s[1:].strip().lower() for s in (profile/'plugins.txt').read_text(encoding='utf-8-sig').splitlines() if s.startswith('*')}
        enabled.update(('fallout4.esm','fallout4_vr.esm'))
        order=[s.strip() for s in (profile/'loadorder.txt').read_text(encoding='utf-8-sig').splitlines() if s.strip().lower() in enabled]
        assert [s.lower() for s in names]==[s.lower() for s in order],'Plugin list does not match MO2 load order'
        glock=edit(operation='plugin',plugin='Glock19x.esp')
        assert glock['weapons'] and all(w['record'].lower().startswith('glock19x.esp:') for w in glock['weapons'])
        assert next(p for p in glock['plugins'] if p['name'].lower()=='fallout4.esm')['role']=='master'
        base=edit(operation='plugin',plugin='MK18.esp')
        index=next(w['index'] for w in base['weapons'] if w['record'].lower()=='mk18.esp:000f9d')
        addon=edit(operation='plugin',plugin='MK18AttachmentOverhaul.esp')
        assert next(w['index'] for w in addon['weapons'] if w['record'].lower()=='mk18.esp:000f9d')==index,'Browsing an override changed the winning record'
        save('dynamic override and master roles')
        empty=edit(operation='plugin',plugin='DLCworkshop01.esm');assert not empty['weapons'],'Empty plugin exposed all weapons'
        dev=edit(operation='mode',mode='development');save('development Glock')
        assert dev['mode']=='development' and dev['plugin'].lower()=='glock19x.esp'
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
        mk=edit(operation='plugin',plugin='MK18AttachmentOverhaul.esp')
        assert mk['plugin'].lower()=='mk18attachmentoverhaul.esp' and any(p['name'].lower()=='mk18.esp' and p['role']=='master' for p in mk['plugins'])
        save('development Empire dependencies')
        mkbase=edit(operation='plugin',plugin='MK18.esp')
        assert mkbase['plugin'].lower()=='mk18.esp'
        assert 'MK18AttachmentOverhaul.esp' not in next(p for p in mkbase['plugins'] if p['name'].lower()=='mk18.esp')['masters']
        save('development base separated from Empire')
        edit(operation='mode',mode='dynamic');edit(operation='plugin',plugin='Glock19x.esp');save('restored dynamic')
        print('PASS MENU source flow, MO2 order/winning overrides, dependency roles, strict plugin filtering, development masters and saved testbed switching',flush=True)
    except Exception:
        (work/'failure.json').write_text(json.dumps(dict(state=command('state'),browser=command('source_browser'),diagnostics=command('diagnostics')),indent=2),encoding='utf-8')
        raise

if __name__=='__main__':run()
