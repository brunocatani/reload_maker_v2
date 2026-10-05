"""Part outlines (rework 03 §2) and the part surfaces behind them (P4-01) on the Glock.

- Every installed part outlines exactly its own shapes, and no shape belongs to
  two parts. NifSkope's moveAllNiBlocks once announced its rows one early, so the
  magazine's last shape (Mag:0) was later read as the sights' Glow:0.
- Focusing a part draws its outline: the render gains pixels of the selection
  colour that the same view without a highlighted part lacks. The magazine's
  shapes are checked by name; it sits inside the grip, so its outline is dimmed.
- The outline pass stays under the plan's 1 ms of GPU time.
"""
from pathlib import Path
import sys

from PIL import Image

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit
from server import command

SELECTION=(0xf0,0xc7,0x85)  # design::color::selection


def viewport():return command('diagnostics')['viewport']


def outline_names(slot):
    edit('focus_slot',dict(slot=slot));ready()
    view=viewport();names={m['block']:m['name'] for m in view['meshes']}
    blocks=[b for h in view['highlights'] for b in h['blocks']]
    return [names.get(b,'?%d'%b) for b in blocks],blocks


def selection_pixels():
    path=command('render',dict(view='iso',fit=True))['path']
    image=Image.open(path).convert('RGB')
    data=image.tobytes()
    return sum(1 for i in range(0,len(data),3) if all(abs(data[i+c]-SELECTION[c])<=24 for c in range(3)))


def run():
    original=ready()['project'];original_bytes=Path(original).read_bytes()
    edit('testbed',dict(name='glock'));session=Path(ready()['project']);session_bytes=session.read_bytes()
    try:
        # Part picking, and with it the outline, is active outside the Reload
        # builder page and the grip tools; Operate is one such page.
        edit('workspace',dict(index=5));state=ready();owners={}
        selected={s['key']:next((c for c in s['choices'] if c['id']==s['selected']),{}) for s in state['slots']}
        for slot,choice in selected.items():
            names,blocks=outline_names(slot)
            for block in blocks:
                assert block not in owners,('shape in two parts',block,owners.get(block),slot)
                owners[block]=slot
            if choice.get('editorId')=='mod_Glock19x_Mag':
                assert sorted(names)==['Bullet:0','Mag:0','MagSecond:0'],names
            if choice.get('editorId','').startswith('mod_Glock19x_Receiver_'):
                receiver=slot
            if choice.get('editorId','').startswith('mod_Glock19x_Scope_'):
                assert sorted(names)==['Glow:0','Sights:0'],names
            if not choice.get('model'):
                assert not blocks,(slot,'a part without a model has shapes',names)
        empty=next(slot for slot,choice in selected.items() if not choice.get('model'))
        edit('focus_slot',dict(slot=empty));ready()
        assert selection_pixels()==0,'Selection colour without a highlighted part'
        # The receiver's slide is in view; the magazine's outline is all the
        # dimmed kind (it sits inside the grip), so it never shows the pure colour.
        edit('focus_slot',dict(slot=receiver));ready()
        outlined=selection_pixels()
        assert outlined>200,('Outline not drawn',outlined)
        for _ in range(3):command('render',dict(view='current'))
        timing=viewport();gpu=timing['highlightGpuMs']
        assert 0<gpu<1.0,('Outline GPU time',gpu)
        # The scene timer measures what one more camera on the scene would cost.
        assert 0<timing['sceneCpuMs']<50 and 0<timing['sceneGpuMs']<50,('Scene timing',timing['sceneCpuMs'],timing['sceneGpuMs'])
        print('PASS part outlines:',len(owners),'shapes in',len(set(owners.values())),'parts; receiver outline',outlined,'px;','%.3f ms'%gpu,
              '; scene %.3f ms CPU, %.3f ms GPU'%(timing['sceneCpuMs'],timing['sceneGpuMs']),flush=True)
    finally:
        session.write_bytes(session_bytes)
        Path(original).write_bytes(original_bytes);edit('open_project',dict(path=original));ready()


if __name__=='__main__':run()
