"""Save all configurations (rework 06 §4) on a copy of the verified RU556 pack.

Every receiver x magazine configuration is saved in one write and the author's
selection comes back; a second run writes nothing; a configuration whose
authored setup changed is saved alone; configurations that cannot be prepared,
or that DIAMOND rejects, are reported without stopping the rest; a cancelled
job leaves the project byte-identical.
"""
import json
from pathlib import Path
import shutil
import sys
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'));sys.path.insert(0,str(ROOT/'tools'))
from template_mcp_test import ready,edit
from server import command
from product_authoring import save_configurations
from product_backend import Backends
from product_export import inspect_project

VERIFIED=ROOT/'workspace/packs/verified-b5bf9bf910a249da8f180b951aa17ae1'


def wait():
    state=command('pack_job')
    end=time.monotonic()+900
    while state.get('phase') in ('running','cancelling'):
        assert time.monotonic()<end,'Save all configurations timed out'
        time.sleep(.1);state=command('pack_job')
    return state


def save_all(project):
    edit('save_all_configurations',dict(project=str(project)))
    state=wait();assert state['phase']=='complete',state
    return state


def statuses(state):
    counts={}
    for row in state['result']['rows']:counts[row['status']]=counts.get(row['status'],0)+1
    return counts


def selection():return {s['key']:s['selected'] for s in ready()['slots']}


def run():
    work=ROOT/'local/save-all-tests'/uuid.uuid4().hex
    shutil.copytree(next(VERIFIED.glob('*/*/RU556/pack.project.json')).parent,work)
    project=work/'pack.project.json'
    original=ready()['project'];original_bytes=Path(original).read_bytes()
    edit('testbed',dict(name='ru556'));session=Path(ready()['project']);session_bytes=session.read_bytes()
    try:
        edit('workspace',dict(index=5));ready();chosen=selection()
        first=save_all(project);counts=statuses(first);exceptions=first['exceptions']
        assert set(counts)=={'saved'} and counts['saved']+len(exceptions)==first['total'],(counts,len(exceptions),first['total'])
        assert all(e['error'] and len(e['parts'])==2 for e in exceptions),exceptions[:2]
        assert inspect_project(project)[1][0]['variants']==counts['saved']
        assert selection()==chosen,'The author selection was not restored'
        assert not list(Path(ROOT/'local/authoring-captures').glob(first['id'])),'The captures outlived the save'
        written=project.read_bytes()
        again=save_all(project)
        assert statuses(again)=={'up-to-date':counts['saved']},statuses(again)
        assert project.read_bytes()==written,'An up-to-date run rewrote the project'
        # The magazine guide belongs to the selected configuration alone.
        guide=command('magazine_guide_state')['settings']
        edit('magazine_guide',dict(operation='save',settings=dict(guide,travel=guide['travel']*1.25)));ready()
        changed=save_all(project)
        assert statuses(changed)=={'saved':1,'up-to-date':counts['saved']-1},statuses(changed)
        row=next(r for r in changed['result']['rows'] if r['status']=='saved')
        current=json.loads(Path(edit('export',{'kind':'runtime_configuration'})['path']).read_text(encoding='utf-8'))
        assert row['selection']==current['selection'],(row['selection'],current['selection'])
        written=project.read_bytes()
        edit('magazine_guide',dict(operation='reset'));ready()
        edit('save_all_configurations',dict(project=str(project)))
        command('pack_job',dict(operation='cancel'),command('revision')['revision'])
        cancelled=wait()
        assert cancelled['phase']=='cancelled' and project.read_bytes()==written,cancelled
        assert selection()==chosen,'A cancelled job left another selection'
        # The bundled save reports a capture DIAMOND rejects and keeps the rest.
        capture=Path(edit('export',{'kind':'runtime_configuration'})['path'])
        bad=json.loads(capture.read_text(encoding='utf-8'));bad['magazine']['grip']['fingerLocalTransforms']=[]
        rejected=work/'rejected.json';rejected.write_text(json.dumps(bad),encoding='utf-8')
        result=save_configurations(project,[rejected,capture],Backends(ROOT/'build-fast/dist/ReloadMakerV2',ROOT))
        assert [r['status'] for r in result['rows']]==['failed','saved'] and result['rows'][0]['error'],result['rows']
        assert inspect_project(project)[1][0]['variants']==counts['saved']
        print('PASS save all configurations:',counts['saved'],'saved,',len(exceptions),'reported, up to date, one changed, cancel, rejected capture:',work,flush=True)
    finally:
        session.write_bytes(session_bytes)
        Path(original).write_bytes(original_bytes);edit('open_project',dict(path=original));ready()


if __name__=='__main__':run()
