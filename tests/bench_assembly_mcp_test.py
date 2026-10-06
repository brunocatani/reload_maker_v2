"""Dev bench weapons that assemble in the game assemble in Maker (2026-10-05).

PPSh-41, MG42 and SA58 FAL: the template defaults are mod collections, whose
lowest-level member is the default. Remington 870: the receiver shape hangs on
the stock model whose slot the receiver owns, so the owner wait is released.
Project 1911: no part is named receiver; the slide hosts the magazine and is
the reload body of the authoring key. Skipped where the dev bench is absent.
"""
from pathlib import Path
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit
from server import command

# folder, weapon EditorID, slots that must hold a part, expectation
BENCH=(
    ('AnotherOne PPSh-41 [Nexus 53839]','PPSh',('Barrel','Grip','Magazine','receiver','Scope'),'collections'),
    ('MG42 and MG34 [Nexus 38066]','MG42MachineGun',('Barrel','Magazine','receiver'),'collections'),
    ('FN FAL - SA58 (Plus Kukri)','Wana_FNFAL',('Barrel','Grip','Handguard','Receiver'),'collections'),
    ('Remington Model 870 Shotgun [Nexus 71711]','Remington870',('Barrel','Grip','receiver'),'order'),
    ('Project 1911 - Colt and Kimber [Nexus 87558]','Weap_Eb_1911',('Frame','Slide','Magazine'),'host'),
)


def devbench():
    for root in (ROOT,*ROOT.parents):
        if (root/'local/devbench').is_dir():return root/'local/devbench'
    return None


def selections(state):
    return {s['name']:next((c for c in s['choices'] if c['id']==s['selected']),None) for s in state['slots']}


def run():
    bench=devbench()
    if bench is None:
        print('SKIP bench assembly: no local/devbench folder',flush=True);return
    original=ready()['project']
    try:
        for folder,editor_id,slots,expectation in BENCH:
            if not (bench/folder).is_dir():
                print('SKIP',folder,'is not on this bench',flush=True);continue
            edit('open_source',dict(path=(bench/folder).as_posix()));state=ready()
            weapon=next(w for w in state['weapons'] if w.get('editorId','').lower()==editor_id.lower())
            edit('select_weapon',dict(index=weapon['index']));state=ready()
            assert not state['lastError'],(editor_id,state['lastError'])
            chosen=selections(state)
            for slot in slots:
                assert chosen.get(slot) and chosen[slot]['name']!='None',(editor_id,slot,'has no part',chosen.get(slot))
            assert len(state['anchors'])>3,(editor_id,'assembled nothing',state['anchors'])
            if expectation=='order':
                lines=command('diagnostics')['text'].splitlines()
                assert any(l.startswith('ORDER') for l in lines),(editor_id,'the owner wait was not released',lines[-12:])
                assert any(l.startswith('RECEIVER') for l in lines),(editor_id,'the receiver was not placed')
            if expectation=='host':
                # The key names the reload parts once Operate has recorded them,
                # which needs the detected preset confirmed. The assignment is
                # discarded with the document; nothing is saved.
                edit('workspace',dict(index=5));ready()
                checklist=command('checklist_state');assert checklist['preset'],(editor_id,'no preset detected')
                edit('checklist',dict(operation='preset',id=checklist['preset']));ready()
                for _ in range(600):
                    checklist=command('checklist_state')
                    if checklist['evaluated'] or checklist['problem']:break
                    time.sleep(.1)
                simulation=command('simulation_state');assert simulation['active'],(editor_id,simulation.get('error'))
                key=checklist['authoringKey']
                assert 'receiver='+chosen['Slide']['record'].lower() in key.lower(),(editor_id,'the slide is not the reload body',key)
                assert 'magazine='+chosen['Magazine']['record'].lower() in key.lower(),(editor_id,key)
            print('PASS',editor_id,expectation,{k:v['name'] for k,v in chosen.items() if k in slots},flush=True)
    finally:
        edit('open_project',dict(path=original));ready()


if __name__=='__main__':run()
