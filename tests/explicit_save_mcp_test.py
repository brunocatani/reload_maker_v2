"""Nothing writes a project except Save (owner, 2026-10-05).

Choosing another part and moving the camera never mark the document. An
authored change marks it, and leaving the weapon for another testbed in an
unattended editor discards it without touching the file. Save writes the file
and clears the mark.
"""
import json
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit
from server import command

TESTBEDS=('glock','ru556','m1a','thanatos','timberwolf','l118a2')


def insertion():return command('weapon_properties_state')['insertion']['value']


def other_insertion():return 'native-curve' if insertion()=='straight' else 'straight'


def has_drafts(record):
    """A saved finger, grip or mechanism draft is content a rebuild drops, so a
    project carrying one is rightly marked after a part change; the check
    needs one without. Magazine grip drafts survive a rebuild."""
    return any(record.get(k) for k in ('gripDraft','mechanismDraft')) or any(record.get('fingerDrafts',[]))


def clean_testbed():
    for name in TESTBEDS:
        edit('testbed',dict(name=name));state=ready()
        if not has_drafts(json.loads(Path(state['project']).read_text(encoding='utf-8'))):return name,state
    raise AssertionError('every corpus project carries a saved draft')


def run():
    original=ready()['project'];original_bytes=Path(original).read_bytes()
    name,state=clean_testbed();project=Path(state['project']);saved=project.read_bytes()
    away='ru556' if name!='ru556' else 'glock'
    try:
        assert state['modified'] is False,'a freshly opened project is marked modified'
        # View state: another choice in a slot with alternatives, then the camera.
        slot=next(s for s in state['slots'] if len(s['choices'])>1 and s['selected'] not in ('','0'))
        other=next(c['id'] for c in slot['choices'] if c['id']!=slot['selected'])
        edit('configure',dict(slot=slot['key'],mod=other));state=ready()
        assert state['modified'] is False,'choosing a part marked the document'
        edit('view',dict(view='top',zoom=2.5));state=ready()
        assert state['modified'] is False,'moving the camera marked the document'
        edit('testbed',dict(name=away));ready()
        assert project.read_bytes()==saved,'leaving the weapon wrote its project'
        # An authored change marks the document; leaving discards it unsaved.
        edit('testbed',dict(name=name));state=ready()
        assert state['modified'] is False and Path(state['project'])==project
        before=insertion();edit('weapon_property',dict(name='insertion',value=other_insertion()));state=ready()
        assert state['modified'] is True,'an authored change did not mark the document'
        assert project.read_bytes()==saved,'an authored change wrote the project'
        edit('testbed',dict(name=away));ready()
        assert project.read_bytes()==saved,'leaving a changed weapon wrote its project'
        edit('testbed',dict(name=name));state=ready()
        assert state['modified'] is False and insertion()==before,'the discarded change came back'
        # Save is the only write.
        edit('weapon_property',dict(name='insertion',value=other_insertion()));assert ready()['modified'] is True
        edit('save_project',dict(path=''));state=ready()
        assert state['modified'] is False,'Save left the document marked'
        assert project.read_bytes()!=saved,'Save did not write the project'
        print('PASS explicit save: view state never marks, authored edits mark, only Save writes',flush=True)
    finally:
        project.write_bytes(saved);edit('open_project',dict(path=str(project)));ready()
        Path(original).write_bytes(original_bytes);edit('open_project',dict(path=original));ready()


if __name__=='__main__':run()
