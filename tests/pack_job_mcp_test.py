"""Export saved ESP projects through the GUI-owned job's MCP command surface.

Exports the latest version of each named ESP project to a ZIP, leaves the
open project untouched, and a cancelled export writes no archive.

Usage: pack_job_mcp_test.py --plugin Glock19x.esp [--plugin ...]
"""
import argparse
import json
from pathlib import Path
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp'))
from server import command


def finish(job_id):
    until=time.monotonic()+600
    while time.monotonic()<until:
        job=command('pack_job')
        assert job['id']==job_id, 'Job was replaced'
        if job['phase'] in ('complete','failed','cancelled'):
            return job
        time.sleep(.2)
    raise AssertionError('Export timed out')


def run():
    parser=argparse.ArgumentParser()
    parser.add_argument('--plugin',action='append',required=True)
    args=parser.parse_args()
    state=command('state')
    before=state['project']
    evidence=ROOT/'evidence/pack-builder'
    evidence.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='mcp-',dir=evidence) as temporary:
        archive=Path(temporary)/'reloads.zip'
        params=dict(operation='export',selections=[dict(plugin=p) for p in args.plugin],archive=str(archive))
        job=command('pack_job',params,state['revision'])
        result=finish(job['id'])
        assert result['phase']=='complete',result
        assert result['result']['plugins'] and archive.is_file(),result['result']
        assert command('state')['project']==before
        cancelled_archive=Path(temporary)/'cancelled.zip'
        job=command('pack_job',dict(params,archive=str(cancelled_archive)),command('revision')['revision'])
        command('pack_job',dict(operation='cancel'))
        cancelled=finish(job['id'])
        assert cancelled['phase']!='complete' or cancelled_archive.is_file(),cancelled
        if cancelled['phase']!='complete':
            assert 'cancelled' in cancelled['error'] and not cancelled_archive.exists(),cancelled
        (evidence/'integration.json').write_text(json.dumps(dict(export=result,cancellation=cancelled,projectPreserved=True),indent=2))
    print('PASS shared GUI/MCP export, project preservation and cancelled export')


if __name__=='__main__':
    run()
