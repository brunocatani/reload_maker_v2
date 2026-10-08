"""Nothing saves except Save; Save commits a dated version of the ESP project.

Choosing another part and moving the camera never mark the weapon, and a
saved version holds no loadout, camera or page. An authored
change marks it, and leaving for another testbed in an unattended editor
discards it without writing a version. Save writes one new version, which
becomes the latest and loads automatically when the ESP is opened again. An
older version can be loaded deliberately; deleting the newest version makes the
previous one the latest again. Every version this check creates is deleted.
"""
import json
from pathlib import Path
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
sys.path.insert(0,str(ROOT/'tools'))
from template_mcp_test import ready,edit
from server import command
import product_library as library

TESTBEDS=('glock','ru556','m1a','thanatos','timberwolf','l118a2')
SAVE_TIMEOUT_SECONDS=1800


def insertion():return command('weapon_properties_state')['insertion']['value']


def other_insertion():return 'native-curve' if insertion()=='straight' else 'straight'


def has_drafts(record):
    """A finger, grip or mechanism draft is content a rebuild drops, so a weapon
    carrying one is rightly marked after a part change; the check needs one
    without. Magazine grip drafts survive a rebuild."""
    return any(record.get(k) for k in ('gripDraft','mechanismDraft')) or any(record.get('fingerDrafts',[]))


def working_record():
    path=command('export',{'kind':'project'},ready()['revision'])['path']
    return json.loads(Path(path).read_text(encoding='utf-8'))


def clean_testbed():
    for name in TESTBEDS:
        edit('testbed',dict(name=name));state=ready()
        if not has_drafts(working_record()):return name,state
    raise AssertionError('every testbed weapon carries a saved draft')


def save(note):
    job=edit('save_project',dict(note=note))
    deadline=time.monotonic()+SAVE_TIMEOUT_SECONDS
    while time.monotonic()<deadline:
        state=command('pack_job')
        if state.get('id')==job['id'] and state.get('phase') in ('complete','failed','cancelled'):
            assert state['phase']=='complete',state
            return command('esp_project')['latest']
        time.sleep(0.5)
    raise TimeoutError('Save did not finish')


def saves():return [s['id'] for s in command('esp_project')['saves']]


def run():
    name,state=clean_testbed();print('testbed',name,flush=True)
    project=command('esp_project');plugin=project['plugin'];existing=saves();created=[]
    away='ru556' if name!='ru556' else 'glock'
    try:
        assert state['modified'] is False,'a freshly opened weapon is marked modified'
        assert project['base']==project['latest'],'opening the ESP did not load its latest version'
        # View state: another choice in a slot with alternatives, then the camera.
        slot=next(s for s in state['slots'] if len(s['choices'])>1 and s['selected'] not in ('','0'))
        other=next(c['id'] for c in slot['choices'] if c['id']!=slot['selected'])
        opened=working_record()
        edit('configure',dict(slot=slot['key'],mod=other));state=ready()
        if state['modified']:
            after=working_record()
            changed=sorted(k for k in set(opened)|set(after) if opened.get(k)!=after.get(k))
            raise AssertionError(('choosing a part marked the weapon',changed))
        edit('view',dict(view='top',zoom=2.5));state=ready()
        assert state['modified'] is False,'moving the camera marked the weapon'
        edit('testbed',dict(name=away));ready()
        edit('testbed',dict(name=name));ready()
        assert saves()==existing,'leaving the weapon wrote a version'
        # An authored change marks the weapon; leaving discards it unsaved.
        before=insertion();edit('weapon_property',dict(name='insertion',value=other_insertion()));state=ready()
        assert state['modified'] is True,'an authored change did not mark the weapon'
        assert command('esp_project')['unsavedWeapons'],'the ESP project does not list the unsaved weapon'
        edit('testbed',dict(name=away));ready()
        edit('testbed',dict(name=name));state=ready()
        assert saves()==existing,'leaving a changed weapon wrote a version'
        assert state['modified'] is False and insertion()==before,'the discarded change came back'
        # Save is the only write: one new version, now the latest.
        changed=other_insertion();edit('weapon_property',dict(name='insertion',value=changed));assert ready()['modified'] is True
        latest=save('explicit save check');created.append(latest)
        current=command('esp_project')
        assert ready()['modified'] is False and not current['unsavedWeapons'],'Save left the weapon marked'
        assert saves()==existing+[latest] and current['base']==latest,'Save did not commit exactly one new latest version'
        # The version holds authoring only: loadout, camera and page stay in the session.
        folder=library.project_folder(ROOT,plugin);snapshot=library.read_snapshot(folder,latest)
        previous=library.read_snapshot(folder,existing[-1])['weapons'] if existing else {}
        written=[e for r,e in snapshot['weapons'].items() if previous.get(r,{}).get('authoring')!=e['authoring']]
        assert written,'Save wrote no weapon record'
        for entry in written:
            kept=set(library.get_record(folder,entry['authoring']))&{'choices','recordChoices','camera','workspace','body','handsPage','animation'}
            assert not kept,('a saved version holds session state',sorted(kept))
        # Reopening the ESP loads the latest version automatically.
        edit('testbed',dict(name=away));ready();edit('testbed',dict(name=name));ready()
        assert insertion()==changed and command('esp_project')['base']==latest,'the latest version did not load automatically'
        # A deliberate older version loads for this session; the latest is unchanged.
        if existing:
            edit('esp_version',dict(operation='load',save=existing[-1]));ready()
            assert insertion()==before and command('esp_project')['latest']==latest,'loading an older version'
        # Deleting the newest version makes the previous one the latest again.
        edit('esp_version',dict(operation='delete',save=latest));ready()
        deadline=time.monotonic()+60
        while latest in saves() and time.monotonic()<deadline:time.sleep(0.2)
        created.remove(latest)
        assert command('esp_project')['latest']==(existing[-1] if existing else None),'deleting the latest version'
        print('PASS explicit save: view state never marks, leaving discards, Save commits one dated latest version, latest loads automatically, older versions load and delete',flush=True)
    finally:
        for save_id in created:
            try:edit('esp_version',dict(operation='delete',save=save_id));ready()
            except RuntimeError as error:print('cleanup',plugin,save_id,error,flush=True)


if __name__=='__main__':run()
