"""Exercise the GUI-owned asynchronous builder through its MCP command surface."""
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
    until=time.monotonic()+180
    while time.monotonic()<until:
        job=command('pack_job')
        assert job['id']==job_id, 'Job was replaced'
        if job['phase'] in ('complete','failed'):
            return job
        time.sleep(.2)
    raise AssertionError('Pack job timed out')


def run():
    parser=argparse.ArgumentParser()
    parser.add_argument('--pack',action='append',required=True)
    args=parser.parse_args()
    state=command('state')
    before=state['project']
    evidence=ROOT/'evidence/pack-builder'
    evidence.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='mcp-',dir=evidence) as temporary:
        params=dict(operation='build',packs=args.pack,installation=temporary,initialize=True)
        job=command('pack_job',params,state['revision'])
        result=finish(job['id'])
        assert result['phase']=='complete',result
        assert Path(result['result']['output'],'DIAMOND_Generated.esp').is_file()
        assert command('state')['project']==before
        pointer=Path(temporary)/'current.json'
        preserved=pointer.read_bytes()
        params['initialize']=False
        job=command('pack_job',params,command('state')['revision'])
        command('pack_job',dict(operation='cancel'))
        cancelled=finish(job['id'])
        assert cancelled['phase']=='failed' and 'cancelled' in cancelled['error'],cancelled
        assert pointer.read_bytes()==preserved, 'Cancellation promoted a partial build'
        (evidence/'integration.json').write_text(json.dumps(dict(build=result,cancellation=cancelled,projectPreserved=True),indent=2))
    print('PASS shared GUI/MCP builder, project preservation and cancelled generation')


if __name__=='__main__':
    run()
