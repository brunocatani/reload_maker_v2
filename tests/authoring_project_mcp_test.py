"""Weapons of one ESP share one in-memory project and one saved version.

The Timberwolf testbed's source holds two weapons of the same plugin. Edits to
both survive switching between them without any save, Save commits both in one
dated version, and after the source is opened again each weapon loads its
latest saved authoring. Every version this check creates is deleted.
"""
import sys
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit
from server import command

SAVE_TIMEOUT_SECONDS=1800


def insertion():return command('weapon_properties_state')['insertion']['value']


def flipped(value):return 'native-curve' if value=='straight' else 'straight'


def saves():return [s['id'] for s in command('esp_project')['saves']]


def select(state,editor):
    edit('select_weapon',dict(index=next(w['index'] for w in state['weapons'] if w['editorId'].lower()==editor)));return ready()


def run():
    edit('testbed',dict(name='timberwolf'));state=ready()
    project=command('esp_project');existing=saves();created=[]
    try:
        # Both weapons belong to the open ESP project.
        state=select(state,'c14timberwolf');first=insertion();edit('weapon_property',dict(name='insertion',value=flipped(first)))
        state=select(ready(),'l118a2');assert command('esp_project')['plugin']==project['plugin'],'the second weapon opened another ESP project'
        second=insertion();edit('weapon_property',dict(name='insertion',value=flipped(second)))
        state=select(ready(),'c14timberwolf')
        assert insertion()==flipped(first),'switching weapons lost the first weapon\'s unsaved edit'
        assert len(command('esp_project')['unsavedWeapons'])==2,command('esp_project')['unsavedWeapons']
        assert saves()==existing,'switching weapons wrote a version'
        # One Save commits both weapons together.
        job=edit('save_project',dict(note='two weapons'))
        deadline=time.monotonic()+SAVE_TIMEOUT_SECONDS
        while time.monotonic()<deadline:
            job_state=command('pack_job')
            if job_state.get('id')==job['id'] and job_state.get('phase') in ('complete','failed','cancelled'):break
            time.sleep(0.5)
        assert job_state.get('phase')=='complete',job_state
        latest=command('esp_project')['latest'];created.append(latest)
        assert saves()==existing+[latest] and not command('esp_project')['unsavedWeapons']
        # Opening the source again loads the latest version for each weapon.
        state=ready();edit('open_source',dict(path=state['source'],gameData=state['gameData']));state=ready()
        state=select(state,'c14timberwolf');assert insertion()==flipped(first) and ready()['modified'] is False
        state=select(state,'l118a2');assert insertion()==flipped(second) and ready()['modified'] is False
        assert command('esp_project')['base']==latest
        print('PASS one ESP project: unsaved edits kept across weapons, one Save commits both, latest reloads after reopening the source',flush=True)
    finally:
        for save_id in created:
            try:edit('esp_version',dict(operation='delete',save=save_id));ready()
            except RuntimeError as error:print('cleanup',save_id,error,flush=True)


if __name__=='__main__':run()
