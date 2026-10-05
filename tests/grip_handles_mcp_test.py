"""Grip placement handles (rework 03 §3) drawn in the GL frame on the Glock.

The move tool shows axis arrows and the pivot square, the rotate tool three
rings plus the screen ring. They are depth-tested against the weapon and the
hands, so the check is that each tool puts its handle colours on screen (a pass
that silently draws nothing fails here), and that grip_tool rejects unknown tools.
"""
from pathlib import Path
import sys

from PIL import Image

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit
from server import command

# GripViewport handle colours: X, Y, Z axes and the pivot / screen ring.
COLORS={'x':(0xef,0x74,0x68),'y':(0x9c,0xca,0x75),'z':(0x6b,0xa5,0xe8),'pivot':(0xef,0xcf,0x99)}


def counts():
    path=command('render',dict(view='iso',fit=True))['path']
    data=Image.open(path).convert('RGB').tobytes()
    result=dict.fromkeys(COLORS,0)
    for i in range(0,len(data),3):
        for name,color in COLORS.items():
            if all(abs(data[i+c]-color[c])<=18 for c in range(3)):result[name]+=1
    return result


def run():
    original=ready()['project'];original_bytes=Path(original).read_bytes()
    edit('testbed',dict(name='glock'));session=Path(ready()['project']);session_bytes=session.read_bytes()
    try:
        state=ready()
        pose=next(p for p in state['handPoses'] if p['hand']=='left')['id']
        edit('start_grip',dict(poseId=pose,anchor=0));ready()
        try:edit('grip_tool',dict(tool='scale'))
        except RuntimeError:pass
        else:raise AssertionError('Unknown grip tool accepted')
        edit('grip_tool',dict(tool='move'));ready();move=counts()
        assert all(move[name]>15 for name in COLORS),('Move handles not drawn',move)
        edit('grip_tool',dict(tool='rotate'));ready();rotate=counts()
        assert all(rotate[name]>60 for name in COLORS),('Rotation rings not drawn',rotate)
        print('PASS grip handles: move',move,'rotate',rotate,flush=True)
    finally:
        session.write_bytes(session_bytes)
        Path(original).write_bytes(original_bytes);edit('open_project',dict(path=original));ready()


if __name__=='__main__':run()
