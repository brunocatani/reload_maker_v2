"""Read-only census of the actual MO2 player-weapon picker and localization."""
import json
from pathlib import Path
import re
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp'))
from server import command


def run():
    deadline=time.monotonic()+180
    while True:
        try:
            state=command('state')
            if not state['busy']:break
        except FileNotFoundError:pass
        if time.monotonic()>deadline:raise AssertionError('Maker library did not become ready')
        time.sleep(.2)
    install=command('installation');assert install['virtualFilesystem'],'Run inside the selected MO2 profile'
    coverage=command('pack_coverage');diagnostics=command('diagnostics')
    report=diagnostics['weaponLibrary'];weapons=state['weapons']
    work=ROOT/'local/player-library';work.mkdir(parents=True,exist_ok=True)
    (work/'after.json').write_text(json.dumps(dict(installation=install,weapons=weapons,coverage=coverage,diagnostics=diagnostics),indent=2),encoding='utf-8')
    assert report['total']==report['shown']+report['hidden'] and report['shown']==len(weapons),report
    assert report['hidden']>0 and coverage['weaponFilter']==report
    assert len(coverage['loadedWeapons'])==len(weapons),'Picker and coverage disagree'
    assert not [w for w in weapons if re.fullmatch(r'String [0-9a-fA-F]{8}',w['name'])]
    by_id={w['editorId']:w for w in weapons}
    for name in ('AssaultronRightBlade','AssaultronLeftBlade','CompCurie_LaserGun_Left','MrGutsy_LaserGun_Right','SentryBotRightShoulderClusterLauncherIndoors'):
        assert name not in by_id,('Internal weapon remains selectable',name)
        assert any(w['editorId']==name and w['reason']=='Non-playable weapon' for w in report['excluded']),('Missing exclusion evidence',name)
    for name in ('Knife','BaseballBat','TireIron'):
        assert name in by_id,('Playable melee removed',name)
        assert by_id[name]['name']!=name,('Localized name did not resolve',name)
    required={'glock19x.esp:000f9b','ru556.esp:000f99','m1a.esp:000f9a','l118a2.esp:00206c','l118a2.esp:000002','diarifles.esp:00059e','mk18.esp:000f9d'}
    actual={w['record'].lower() for w in coverage['loadedWeapons']}
    assert required<=actual,('Existing testbed disappeared',sorted(required-actual))
    assert {'SREP_ServiceRifle','SREP_ServiceRifle_Unique_HoneyBadger','SREP_AR10','SREP_XM16E1'}<=set(by_id),'SREP testbed removed'
    assert 'LOCALIZATION strings/fallout4_en.strings:' in diagnostics['text'],diagnostics['text'][:1000]
    print(json.dumps(dict(ok=True,shown=report['shown'],total=report['total'],hidden=report['hidden'],reasons=report['reasons'],
        melee={name:by_id[name]['name'] for name in ('Knife','BaseballBat','TireIron')},evidence=str(work/'after.json'))),flush=True)


if __name__=='__main__':run()
