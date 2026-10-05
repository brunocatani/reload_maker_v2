"""Review marks follow authored data, persist, and ignore viewport-only changes."""
import json
from pathlib import Path
import sys,uuid
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tests'))
from visible_geometry_mcp_test import ready,edit
from server import command

def statuses():return {s['index']:s for s in command('roadmap')['steps']}

def run():
    work=ROOT/'local/roadmap-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=ready()['project'];original_bytes=Path(original).read_bytes()
    try:
        edit('testbed',dict(name='glock'));ready();project=Path(ready()['project']);saved=project.read_bytes()
        try:
            edit('save_project',dict(path=str(work/'draft.project.json')))
            edit('workspace',dict(index=5));ready();edit('roadmap',dict(operation='template',id=command('roadmap')['template']));ready()
            edit('roadmap',dict(operation='record-sources'))
            for step in range(10):
                state=statuses()[step];assert state['canReview'],(step,state)
                edit('roadmap',dict(operation='review',index=step))
            before=statuses();edit('view',dict(view='side',fit=True));assert statuses()==before,'Camera changed review state'
            guide=command('magazine_guide_state')['settings'];guide['travel']*=1.2
            edit('magazine_guide',dict(operation='save',settings=guide));ready();changed=statuses()
            for step in range(10):assert changed[step]['status']==('needs-review' if step in (6,9) else 'reviewed'),(step,changed[step])
            edit('magazine_guide',dict(operation='reset'));ready();assert all(statuses()[i]['status']=='reviewed' for i in range(10))
            for empty in (False,True):
                edit('workspace',dict(index=5));ready();edit('capture_magazine_world',dict(empty=empty));ready('collision_state')
            assert statuses()[10]['canReview'],statuses()[10]
            edit('roadmap',dict(operation='review',index=10));reviewed=statuses()[10]
            edit('collisions',dict(operation='view',overlay=False));assert statuses()[10]==reviewed,'Overlay invalidated collision review'
            edit('collisions',dict(operation='hull',index=0,enabled=False));assert statuses()[10]['status']=='needs-review'
            edit('collisions',dict(operation='hull',index=0,enabled=True));assert statuses()[10]['canReview']
            edit('roadmap',dict(operation='review',index=10))
            edit('workspace',dict(index=5));ready();edit('roadmap',dict(operation='step',index=7));edit('save_project',dict(path=str(work/'reviewed.project.json')))
            before=statuses();edit('open_project',dict(path=str(work/'reviewed.project.json')));ready();after=statuses()
            assert after==before,'Source reopen lost review fingerprints';assert command('roadmap')['selectedStep']==7
            edit('roadmap',dict(operation='unreview',index=7));assert statuses()[7]['status']=='ready'
            (work/'result.json').write_text(json.dumps(dict(ok=True,steps=after),indent=2));print('PASS native readiness, precise guide/collision invalidation, camera independence and project resume:',work,flush=True)
        finally:
            project.write_bytes(saved);command('open_project',dict(path=str(project)),command('revision')['revision']);ready()
            edit('save_project',dict(path=str(project)));project.write_bytes(saved)
    finally:
        Path(original).write_bytes(original_bytes);command('open_project',dict(path=original),command('revision')['revision']);ready()
        edit('save_project',dict(path=original));Path(original).write_bytes(original_bytes)

if __name__=='__main__':run()
