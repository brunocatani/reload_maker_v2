"""Point-and-note pins (rework 03 §4) on the Glock.

A pin added at a viewport point anchors to the part shape under it, is recorded
as written by the AI when a command adds it, is drawn where it was placed and
follows the slide through a pull. Notes can be edited, resolved with a
reference and reopened. Pins are saved with the project and reopen unchanged.
A pin whose part is swapped out stays listed but is not placed. Bad requests
are rejected.
"""
from pathlib import Path
import json
import shutil
import sys
import uuid

from PIL import Image, ImageChops

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit
from server import command


def pins():return command('annotations')['annotations']


def render():return Image.open(command('render',dict(view='current'))['path']).convert('RGB')


def rejected(params,reason):
    try:edit('annotation',params)
    except RuntimeError:return
    raise AssertionError(reason)


def points_on(label):
    size=command('part_at',dict(x=0,y=0))
    for fy in range(30,71,3):
        for fx in range(15,86,3):
            x,y=size['width']*fx/100,size['height']*fy/100
            if command('part_at',dict(x=x,y=y))['label']==label:yield x,y


def pin_on_slide(text):
    """Adds a pin on the Glock's slide: the receiver slot also holds the frame,
    which does not move."""
    for x,y in points_on('Receiver'):
        state=edit('annotation',dict(operation='add',x=x,y=y,text=text))
        pin=next(p for p in state['annotations'] if p['id']==state['id'])
        if pin['shape'].lower().startswith('slide'):return state,x,y
        edit('annotation',dict(operation='remove',id=pin['id']))
    raise AssertionError('No point on the slide')


def run():
    original=command('export',{'kind':'project'},ready()['revision'])['path'];original_bytes=Path(original).read_bytes()
    edit('testbed',dict(name='glock'));session=Path(command('export',{'kind':'project'},ready()['revision'])['path']);session_bytes=session.read_bytes()
    work=ROOT/'local/pin-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    try:
        edit('workspace',dict(index=5));ready()
        command('render',dict(view='side',fit=True))
        assert pins()==[],('Testbed already has pins',pins())
        rejected(dict(operation='add',x=2,y=2,text='background'),'A pin off the weapon was accepted')
        x,y=next(points_on('Receiver'))
        rejected(dict(operation='add',x=x,y=y,text='   '),'A blank note was accepted')
        rejected(dict(operation='add',x=x,y=y,text='x'*2001),'An over-long note was accepted')
        rejected(dict(operation='explode',id='x'),'An unknown operation was accepted')
        rejected(dict(operation='edit',id='missing',text='note'),'An unknown pin was edited')

        plain=render()
        state,x,y=pin_on_slide('Ejection port edge: the case should clear it')
        pin=state['annotations'][0]
        assert state['id']==pin['id'] and state['selected']==pin['id']
        assert pin['author']=='ai' and pin['state']=='open' and pin['placed'] and pin['slotLabel']=='Receiver' and pin['shape'],pin
        assert abs(pin['screen'][0]-x)<3 and abs(pin['screen'][1]-y)<3,('Pin is not where it was placed',pin['screen'],(x,y))
        changed=ImageChops.difference(plain,render()).convert('L').point(lambda v:255 if v>24 else 0).getbbox()
        scale=plain.width/command('part_at',dict(x=0,y=0))['width']
        assert changed and changed[0]<=x*scale<=changed[2] and changed[1]<=y*scale<=changed[3],('Pin not drawn at its point',changed,(x,y))

        # Weapon space, so the check holds whatever pose and camera an earlier
        # suite left behind.
        edit('simulate',dict(operation='pull',progress=1.0))
        pulled=pins()[0]['position']
        travel=sum((a-b)**2 for a,b in zip(pulled,pin['position']))**.5
        assert travel>1.0,('Pin does not follow the slide',pin['position'],pulled)
        edit('simulate',dict(operation='release'))

        edited=edit('annotation',dict(operation='edit',id=pin['id'],text='  Port edge: the spent case must clear it  '))['annotations'][0]
        assert edited['text']=='Port edge: the spent case must clear it',edited
        resolved=edit('annotation',dict(operation='resolve',id=pin['id'],reference='ejection.port'))['annotations'][0]
        assert resolved['state']=='resolved' and resolved['resolution']=='ejection.port',resolved
        reopened=edit('annotation',dict(operation='reopen',id=pin['id']))['annotations'][0]
        assert reopened['state']=='open' and 'resolution' not in reopened,reopened

        saved=work/'pins.project.json'
        edit('export',dict(kind='project',path=str(saved)));stored=json.loads(saved.read_text(encoding='utf-8'))['annotations']
        assert len(stored)==1 and stored[0]['id']==pin['id'] and 'screen' not in stored[0],stored
        edit('import_project',dict(path=str(session)));ready()
        assert pins()==[],'Pins leaked into another project'
        edit('import_project',dict(path=str(saved)));ready()
        restored=pins()
        assert len(restored)==1 and {k:restored[0][k] for k in ('id','text','author','state','shape','slot','part')}=={k:reopened[k] for k in ('id','text','author','state','shape','slot','part')},(restored,reopened)

        # Swapping the part out keeps the pin listed but unplaced.
        receiver=next(slot for slot in ready()['slots'] if slot['name']=='Receiver')
        other=next((choice for choice in receiver['choices'] if choice['id']!=receiver['selected']),None)
        if other:
            edit('configure',dict(slot=receiver['key'],mod=other['id']));ready()
            swapped=pins()[0]
            assert not swapped['placed'] and 'screen' not in swapped,swapped
            edit('configure',dict(slot=receiver['key'],mod=receiver['selected']));ready()
            assert pins()[0]['placed'],'Pin not placed again with its part back'

        state=edit('annotation',dict(operation='remove',id=pin['id']))
        assert state['annotations']==[] and state['selected']=='',state
        edit('export',dict(kind='project',path=str(saved)))
        assert 'annotations' not in json.loads(saved.read_text(encoding='utf-8')),'An empty pin list was saved'
        print('PASS pins: anchored to',pin['shape'],'in',pin['slotLabel'],'moved',round(travel,2),'units with the slide',
              'swap checked' if other else 'no alternative receiver',flush=True)
    finally:
        # Saving elsewhere re-points the Glock testbed and the last project at
        # the temporary file; saving the originals again points them back.
        session.write_bytes(session_bytes)
        edit('import_project',dict(path=str(session)));ready()
        edit('export',dict(kind='project',path=str(session)));session.write_bytes(session_bytes)
        Path(original).write_bytes(original_bytes);edit('import_project',dict(path=original));ready()
        edit('export',dict(kind='project',path=original));Path(original).write_bytes(original_bytes)
        shutil.rmtree(work,ignore_errors=True)


if __name__=='__main__':run()
