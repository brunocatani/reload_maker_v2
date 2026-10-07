"""Save captures every configuration of an edited weapon (RU556 testbed).

Save prepares and captures each receiver x magazine configuration of the edited
weapon, commits them with its authoring in one new version, and brings back the
author's selection and Operate state. Configurations that cannot be prepared are
reported without stopping the rest. A cancelled save writes no version and
keeps the unsaved edit. DIAMOND rejecting one capture is reported while the
others are kept. Every version this check creates is deleted.
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


def wait(job):
    end=time.monotonic()+1800
    while True:
        state=command('pack_job')
        if state.get('id')==job['id'] and state.get('phase') not in ('running','cancelling'):return state
        assert time.monotonic()<end,'Save timed out'
        time.sleep(.2)


def selection():return {s['key']:s['selected'] for s in ready()['slots']}


def saves():return [s['id'] for s in command('esp_project')['saves']]


def run():
    edit('testbed',dict(name='ru556'));ready();existing=saves();created=[]
    try:
        edit('workspace',dict(index=5));ready();chosen=selection()
        guide=command('magazine_guide_state')['settings']
        edit('magazine_guide',dict(operation='save',settings=dict(guide,travel=guide['travel']*1.25)));ready()
        assert ready()['modified'] is True
        # A cancelled save writes nothing and keeps the edit.
        job=edit('save_project',dict(note='cancelled'))
        command('pack_job',dict(operation='cancel'))
        cancelled=wait(job)
        assert cancelled['phase'] in ('cancelled','failed') and saves()==existing,cancelled
        assert selection()==chosen and ready()['modified'] is True,'a cancelled save lost the selection or the edit'
        # A completed save captures every configuration and restores the selection.
        job=edit('save_project',dict(note='captures'));done=wait(job)
        assert done['phase']=='complete',done
        latest=command('esp_project')['latest'];created.append(latest)
        report=next(iter(done['result']['packReports'].values()))
        assert report['saved']>0 and all(r['status'] in ('saved','failed') for r in report['rows']),report
        assert all(e.get('error') for e in report['exceptions']),report['exceptions'][:2]
        assert selection()==chosen,'Save did not restore the author selection'
        assert ready()['modified'] is False
        assert not (ROOT/'local/tmp/library-save'/job['id']).exists(),'the save folder outlived the save'
        weapon=next(w for w in next(s for s in command('esp_project')['saves'] if s['id']==latest)['weapons'] if w['exportable'])
        assert weapon['configurations']>=report['saved'],weapon
        # Nothing changed: there is nothing to save.
        try:edit('save_project',dict(note='again'))
        except RuntimeError as error:assert 'Nothing to save' in str(error),error
        else:raise AssertionError('an unchanged project saved another version')
        # The bundled save reports a capture DIAMOND rejects and keeps the rest.
        work=ROOT/'local/save-capture-tests'/uuid.uuid4().hex
        shutil.copytree(next(VERIFIED.glob('*/*/RU556/pack.project.json')).parent,work)
        project=work/'pack.project.json';variants=inspect_project(project)[1][0]['variants']
        capture=Path(edit('export',{'kind':'runtime_configuration'})['path'])
        bad=json.loads(capture.read_text(encoding='utf-8'));bad['magazine']['grip']['fingerLocalTransforms']=[]
        rejected=work/'rejected.json';rejected.write_text(json.dumps(bad),encoding='utf-8')
        result=save_configurations(project,[rejected,capture],Backends(ROOT/'build-fast/dist/ReloadMakerV2',ROOT))
        assert [r['status'] for r in result['rows']]==['failed','saved'] and result['rows'][0]['error'],result['rows']
        assert inspect_project(project)[1][0]['variants']>=variants
        print('PASS Save captures:',report['saved'],'configurations saved,',report['failed'],'reported, selection restored, cancel writes nothing, rejected capture reported',flush=True)
    finally:
        edit('magazine_guide',dict(operation='reset'));ready()
        for save_id in created:
            try:edit('esp_version',dict(operation='delete',save=save_id));ready()
            except RuntimeError as error:print('cleanup',save_id,error,flush=True)


if __name__=='__main__':run()
