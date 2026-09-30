"""Real source import, sound transitions, native audio submission and persistence."""
import json,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'mcp'))
from server import command
OUT=ROOT/'evidence/sounds';OUT.mkdir(parents=True,exist_ok=True)
def edit(m,**p):
    for attempt in range(5):
        try:return command(m,p,command('state')['revision'])
        except RuntimeError as e:
            if 'Stale document revision' not in str(e) or attempt==4:raise
def ready():
    end=time.monotonic()+60
    while time.monotonic()<end:
        s=command('state')
        if not s['busy']:
            assert not s['lastError'],s['lastError'];return s
        time.sleep(.04)
    raise AssertionError('Load did not complete')
def op(operation,**p):return edit('simulate',operation=operation,**p)
def count():return command('sound_state')['eventsTriggered']
original=ready()['project'];backup=OUT/'original.project.json';edit('save_project',path=str(backup));original_bytes=backup.read_bytes()
muted=command('sound_state')['muted'];results={}
try:
    for weapon in ['glock','ru556','m1a','timberwolf']:
        edit('testbed',name=weapon);ready();edit('workspace',index=6);ready();op('reset')
        scratch=OUT/(weapon+'.project.json');edit('save_project',path=str(scratch));saved=scratch.read_bytes()
        try:
            s=command('sound_state');assert len(s['profile']['cues'])==12 and not s['error'],s
            dry=next(c for c in s['profile']['cues'] if c['id']=='dry');assert dry['enabled'] and dry['record']=='Fallout4.esm:01dacd'
            edit('sound',operation='mute',enabled=False);before=s['audio']['played'];played=edit('sound',operation='play',id='pull');assert played['audio']['ready'] and played['audio']['played']==before+1 and not played['error']
            edit('sound',operation='stop');assert command('sound_state')['audio']['activeVoices']==0
            edit('sound',operation='mute',enabled=True)
            before=count();op('pull',progress=.6);after=count();assert after>before,'Manual pull emitted no cue'
            op('pull',progress=.6);assert count()==after,'Holding repeated a sound'
            op('release',cancel=True);assert count()==after,'Cancel replayed sound'
            op('drop_magazine');op('pull',progress=1)
            if weapon=='timberwolf':op('pull',progress=0)
            op('release');op('step',seconds=2)
            state=command('simulation_state');assert not state['ammunition']['chambered'] and state['slide']==0,state
            before=count();op('trigger_down');assert count()==before+1;op('trigger_down');op('step',seconds=2);assert count()==before+1,'Held empty trigger spammed'
            op('trigger_up');op('trigger_down');assert count()==before+2;op('trigger_up')
            op('reset');before=count();op('mag_out');op('step',seconds=2);assert count()>before,'Magazine withdrawal emitted no cue'
            before=count();op('mag_in');op('step',seconds=2);assert count()>before,'Magazine seating emitted no cue'
            op('reset');before=count();op('reload');op('step',seconds=12);assert count()>before,'Native source reload is silent'
            old=next(c for c in command('sound_state')['profile']['cues'] if c['id']=='pull')
            edit('sound',operation='edit',id='pull',gain=.65,progress=.12);edit('sound',operation='save');edit('open_project',path=str(scratch));ready()
            cue=next(c for c in command('sound_state')['profile']['cues'] if c['id']=='pull');assert abs(cue['gain']-.65)<1e-5 and abs(cue['progress']-.12)<1e-5
            exported=edit('export',kind='sounds');profile=json.loads(Path(exported['path']).read_text(encoding='utf-8'));assert profile==command('sound_state')['profile']
            results[weapon]={'enabled':sum(c['enabled'] for c in profile['cues']),'samples':played['audio']['cachedSamples'],'descriptor':old['record']}
            print('PASS',weapon,'native audio submission, movement/dry-fire/source cues, save/reopen/export',flush=True)
        finally:
            scratch.write_bytes(saved);edit('open_project',path=str(scratch));ready()
    (OUT/'validated.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
finally:
    backup.write_bytes(original_bytes);edit('open_project',path=str(backup));ready();edit('save_project',path=original);edit('sound',operation='mute',enabled=muted)
