"""Native-motion ghost in Operate (rework 03 §5).

With the ghost shown, a part whose authored motion differs from the source clip
is drawn again, tinted, at the clip's pose. The SREP Service rifle's charging
handle is the reference: the source clip leaves it forward while Maker derives
its stroke from the bolt, so a part pull shows one ghost along the bore and a
closed handle shows none. The Glock's slide follows its own clip, so it never
has a ghost. The option is off by default and clears when switched off or when
Operate is left.
"""
from pathlib import Path
import sys

from PIL import Image, ImageChops

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit
from server import command


def ghosts():return command('diagnostics')['viewport']['ghosts']


def render():return Image.open(command('render',dict(view='side',fit=True))['path']).convert('RGB')


def operate(name):
    edit('testbed',dict(name=name));ready();edit('workspace',dict(index=5));ready()


def run():
    original=command('export',{'kind':'project'},ready()['revision'])['path'];original_bytes=Path(original).read_bytes()
    edit('testbed',dict(name='srep-service'));session=Path(command('export',{'kind':'project'},ready()['revision'])['path']);session_bytes=session.read_bytes()
    try:
        operate('srep-service')
        # Switching the option is a view change: at rest it must leave the
        # simulation ready (an earlier build grabbed the handle here).
        ready_state=edit('simulate',dict(operation='reset'))
        toggled=edit('simulate',dict(operation='ghost',enabled=True))
        assert toggled['action']==ready_state['action'] and toggled['slide']==ready_state['slide'],('Toggling the ghost changed the simulation',ready_state['action'],toggled['action'])
        edit('simulate',dict(operation='ghost',enabled=False))
        assert edit('simulate',dict(operation='fire'))['action']!=ready_state['action'],'The weapon does not fire after toggling the ghost'
        for _ in range(80):
            if edit('simulate',dict(operation='step',seconds=.05))['action']==ready_state['action']:break
        state=edit('simulate',dict(operation='pull',progress=.6))
        assert state['nativeGhost'] is False and not ghosts(),('Ghost not off by default',state['nativeGhost'],ghosts())
        plain=render()
        state=edit('simulate',dict(operation='ghost',enabled=True))
        assert state['nativeGhost'] is True,state
        shown=ghosts()
        assert len(shown)==1 and shown[0]['blocks'],('Charging handle ghost missing',shown)
        offset=[abs(v) for v in shown[0]['offset']]
        # The handle stays forward in the clip: the offset runs along the bore.
        assert max(offset)>1 and sorted(offset)[1]<.05,('Ghost offset is not along one axis',shown[0]['offset'])
        changed=ImageChops.difference(plain,render()).convert('L').point(lambda v:255 if v>12 else 0).getbbox()
        assert changed,'Ghost draws nothing'
        edit('simulate',dict(operation='pull',progress=0))
        assert not ghosts(),('Closed handle still has a ghost',ghosts())
        edit('simulate',dict(operation='pull',progress=.6))
        assert len(ghosts())==1
        edit('workspace',dict(index=2));ready()
        assert not ghosts(),('Ghost left after Operate',ghosts())

        operate('glock')
        edit('simulate',dict(operation='pull',progress=.6))
        assert not ghosts(),('Glock slide follows its own clip',ghosts())

        operate('srep-service')
        edit('simulate',dict(operation='pull',progress=.6))
        assert len(ghosts())==1
        state=edit('simulate',dict(operation='ghost',enabled=False))
        assert state['nativeGhost'] is False and not ghosts(),('Ghost not cleared',ghosts())
        print('PASS native ghost: handle offset',[round(v,2) for v in shown[0]['offset']],'changed area',changed,flush=True)
    finally:
        try:edit('simulate',dict(operation='ghost',enabled=False))
        except RuntimeError:pass
        session.write_bytes(session_bytes)
        Path(original).write_bytes(original_bytes);edit('import_project',dict(path=original));ready()


if __name__=='__main__':run()
