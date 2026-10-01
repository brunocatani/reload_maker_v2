"""Exercise saved roadmap/source evidence through the real editor controller."""
import copy
import json
from pathlib import Path
import sys
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'mcp'))
from server import command


def ready():
    deadline=time.monotonic()+120
    while time.monotonic()<deadline:
        state=command('state')
        if not state['busy']:
            if state['lastError']:raise RuntimeError(state['lastError'])
            return state
        time.sleep(.1)
    raise RuntimeError('Editor capture did not complete')


def edit(method,params):
    return command(method,params,ready()['revision'])


def main():
    work=ROOT/'local/roadmap-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=ready()['project'];before=work/'before.project.json'
    edit('save_project',{'path':str(before)})
    original_bytes=before.read_bytes()
    try:
        edit('workspace',{'index':5});ready()
        state=edit('roadmap',{'operation':'record-sources'})
        assert state['sourcesRecorded'] and len(state['steps'])==12
        report=command('roadmap',{'operation':'check-sources'})
        assert report['compatible'] and report['resources']
        key=state['key']
        edit('roadmap',{'operation':'step','index':8})
        saved=work/'recorded.project.json';edit('save_project',{'path':str(saved)})
        record=json.loads(saved.read_text())
        assert record['authoringRoadmaps'][key]['step']==8
        stale=copy.deepcopy(record)
        stale['authoringRoadmaps'][key]['sources']['resources'][0]['sha256']='0'*64
        stale_path=work/'stale.project.json';stale_path.write_text(json.dumps(stale))
        edit('open_project',{'path':str(stale_path)});ready()
        assert command('roadmap',{'operation':'state'})['selectedStep']==8
        result=command('roadmap',{'operation':'check-sources'})
        assert not result['compatible'] and result['changed']==1 and result['missing']==0
        # Source review must not discard authored poses or alter their data.
        preserved=work/'after-check.project.json';edit('save_project',{'path':str(preserved)})
        after=json.loads(preserved.read_text())
        for field in ('magazineGrabGrips','magazineGrabDefaults','operatingGrips','grabLocations','soundProfiles'):
            assert after[field]==record[field],field
        (work/'result.json').write_text(json.dumps(dict(ok=True,resources=len(report['resources']),
            checks=['source byte comparison','source mode','saved step round trip','stale digest detected','authored data retained']),indent=2))
        print('PASS roadmap source evidence, persistence, stale-byte detection and authoring preservation:',work)
    finally:
        before.write_bytes(original_bytes)
        edit('open_project',{'path':str(before)});ready()
        if original:edit('save_project',{'path':original})


if __name__=='__main__':main()
