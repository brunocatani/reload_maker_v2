"""Inspect one pack configuration against the actual fresh editor source view."""
import copy
import json
from pathlib import Path
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from visible_geometry_mcp_test import ready,edit,finish
from server import command

def main():
    work=ROOT/'local/source-audit-mcp'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=ready()['project'];saved=Path(original).read_bytes()
    try:
        edit('testbed',{'name':'glock'});ready();edit('workspace',{'index':5});ready()
        exported=json.loads(Path(edit('export',{'kind':'runtime_configuration'})['path']).read_text())
        evidence=dict(version=1,resources=exported['runtimeSources'])
        before=command('state')
        audit=command('inspect_sources',dict(evidence=evidence));assert audit['compatible'],audit
        assert audit['sourceMode']=='ordered-packages'
        wrong=copy.deepcopy(evidence);wrong['resources'][0]['sha256']='0'*64
        stale=command('inspect_sources',dict(evidence=wrong));assert not stale['compatible'] and stale['changed']==1
        after=command('state');assert before['revision']==after['revision'] and before['handPoses']==after['handPoses']
        index=json.loads((ROOT/'workspace/packs/verified-b5bf9bf910a249da8f180b951aa17ae1/projects.json').read_text())
        project=next(p['path'] for p in index if p['name']=='Glock')
        inspected=finish(edit('pack_job',dict(operation='inspect-project',project=project)))
        assert len(inspected['sourceRequirements'][0]['configurations'])==196 and not inspected['sourceConflicts']
        (work/'result.json').write_text(json.dumps(dict(ok=True,audit=audit,stale=stale),indent=2))
        print('PASS fresh source inspection, scoped pack requirements and unchanged editor authoring:',work)
    finally:
        Path(original).write_bytes(saved);command('open_project',{'path':original},command('state')['revision']);ready()

if __name__=='__main__':main()
