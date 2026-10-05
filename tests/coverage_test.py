"""Workspace coverage keeps independent packs and isolates damaged drafts."""
from pathlib import Path
import json,os,sys,uuid
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
from product_coverage import inventory

def run():
    work=ROOT/'local/coverage-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    source=ROOT/'workspace/packs/verified-b5bf9bf910a249da8f180b951aa17ae1/projects.json'
    paths=[p['path'] for p in json.loads(source.read_text())]
    bad=work/'workspace/packs/unfinished/pack.project.json';bad.parent.mkdir(parents=True);bad.write_text('[]')
    broken=work/'workspace/packs/broken/pack.project.json';broken.parent.mkdir(parents=True)
    draft=json.loads(Path(paths[0]).read_text());broken.write_text(json.dumps(draft)) # Relative immutable inputs are deliberately missing here.
    for context in ('base','overhaul'):
        editor=work/'workspace/projects/example.esp/000800'/context/'authoring.project.json';editor.parent.mkdir(parents=True)
        editor.write_text(json.dumps(dict(format=1,kind='reload-maker-v2-project',weapon='Example.esp:000800',weaponName='Example rifle',library=True,mo2Profile=context)))
    result=inventory(work,paths+[paths[0]])
    assert len(result['authoringProjects'])==2 and {p['context'] for p in result['authoringProjects']}=={'base','overhaul'}
    assert all(p['kind']=='authoring' and p['weapons'][0]['record']=='example.esp:000800' for p in result['authoringProjects'])
    valid=[p for p in result['projects'] if p['status']=='saved'];errors=[p for p in result['projects'] if p['status']=='invalid']
    assert len(valid)==12 and len(errors)==2 and all(p['error'] for p in errors)
    recognized=next(p for p in errors if Path(p['project'])==broken)
    assert recognized['weapons'][0]['record']==draft['weapons'][0]['record'].lower()
    assert 'variants' not in recognized['weapons'][0],'Damaged draft invented a configuration count'
    assert sum(w['variants'] for p in valid for w in p['weapons'])==852
    assert sum(w['magazines'] for p in valid for w in p['weapons'])==135
    assert len({w['record'] for p in valid for w in p['weapons']})<12,'Fixture must include independent packs for one weapon'
    assert result['gameEnablement']=='not-checked'
    marker=work/'cancel';marker.touch();os.environ['RELOAD_MAKER_CANCEL_FILE']=str(marker)
    try:
        try:inventory(work,paths)
        except RuntimeError:pass
        else:raise AssertionError('Cancelled inventory continued')
    finally:os.environ.pop('RELOAD_MAKER_CANCEL_FILE',None)
    (work/'result.json').write_text(json.dumps(result,indent=2));print('PASS twelve packs, independent same-weapon projects, 852 configurations, 135 fits, isolated damage and cancellation:',work)

if __name__=='__main__':run()
