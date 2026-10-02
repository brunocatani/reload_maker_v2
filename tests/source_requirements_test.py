"""Source compatibility is scoped to a variant; stale drafts remain editable."""
import copy
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from product_export import source_requirements,source_conflicts
from product_packs import read

def main():
    index=read(ROOT/'workspace/packs/verified-b5bf9bf910a249da8f180b951aa17ae1/projects.json')
    first=Path(index[0]['path']);project=read(first);weapon=project['weapons'][0]
    profiles=read(first.parent/weapon['profiles']['path'])
    requirements=source_requirements(weapon,profiles)
    assert len(requirements)==196 and not source_conflicts(requirements)
    for variant,row in zip(profiles['profiles'],requirements):
        assert row['receiver']==variant['receiver'].lower() and row['magazine']==variant['magazine'].lower()
        assert any(resource['path']==variant['source']['animation'].lower() and resource['sha256']==variant['source']['animationSha256'] for resource in row['resources'])
    modified=copy.deepcopy(profiles);modified['profiles'][0]['source']['animationSha256']='1'*64
    mixed=source_requirements(weapon,modified)
    assert mixed[1:]==requirements[1:] and source_conflicts(mixed)==[modified['profiles'][0]['source']['animation'].lower()]
    edited=copy.deepcopy(weapon);selected={k:profiles['profiles'][0][k] for k in ('receiver','magazine')}
    edited['captures']=[dict(selection=selected,sources=dict(resources=[dict(kind='model',path='meshes/weapons/source.nif',sha256='2'*64)]))]
    captured=source_requirements(edited,profiles)
    assert captured[1:]==requirements[1:] and len(captured[0]['resources'])==len(requirements[0]['resources'])+1
    edited['captures'][0]['sources']['resources'][0]['path']='../outside.nif'
    try:source_requirements(edited,profiles)
    except ValueError:pass
    else:raise AssertionError('Unsafe source resource accepted')
    print('PASS exact variant association, editable mixed-source drafts, publication conflict reporting and bounded source paths')

if __name__=='__main__':main()
