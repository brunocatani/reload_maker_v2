"""Weapon switching saves independent per-ESP/context projects and resumes them."""
import configparser,json,sys,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit,simulation
from server import command

def run():
    work=ROOT/'local/authoring-project-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    original=ready()['project'];original_bytes=Path(original).read_bytes()
    settings=configparser.ConfigParser(interpolation=None);settings.read(ROOT/'local/settings/ReloadMakerV2/ReloadMakerV2.ini',encoding='utf-8')
    old_paths={name:Path(settings.get('testbeds',name)) for name in ('timberwolf','l118a2')}
    old_bytes={name:p.read_bytes() for name,p in old_paths.items()};destinations={}
    try:
        edit('testbed',dict(name='timberwolf'));state=ready();edit('workspace',dict(index=5));ready()
        before=simulation();first=Path(state['suggestedProject']);assert '/workspace/projects/l118a2.esp/' in first.as_posix().lower()
        destinations[first]=first.read_bytes() if first.exists() else None
        edit('save_project',dict(path=str(first)))
        second_index=next(w['index'] for w in state['weapons'] if w['editorId'].lower()=='l118a2')
        # Find the second canonical path without losing its existing file.
        # Its source context is shared with Timberwolf; record IDs differ.
        coverage=command('pack_coverage');second_key=next(w['record'] for w in coverage['loadedWeapons'] if w['selection']==second_index)
        second=first.parent.parent.parent/second_key.split(':')[1]/first.parent.name/'authoring.project.json'
        destinations[second]=second.read_bytes() if second.exists() else None
        second.parent.mkdir(parents=True,exist_ok=True);second.write_bytes(b'{}')
        try:edit('select_weapon',dict(index=second_index))
        except RuntimeError:pass
        else:raise AssertionError('Invalid saved destination replaced the current project')
        rejected=command('state');assert Path(rejected['project'])==first and rejected['weaponIndex']==state['weaponIndex']
        if destinations[second] is None:second.unlink()
        else:second.write_bytes(destinations[second])
        command('select_weapon',dict(index=second_index),rejected['revision']);state=ready()
        assert Path(state['suggestedProject'])==second
        edit('save_project',dict(path=str(second)));second_record=json.loads(second.read_text(encoding='utf-8'))
        assert second_record['weapon'].lower()==second_key and second!=first
        first_record=json.loads(first.read_text(encoding='utf-8'))
        assert first_record['weapon']!=second_record['weapon']
        first_index=next(w['index'] for w in state['weapons'] if w['editorId'].lower()=='c14timberwolf')
        edit('select_weapon',dict(index=first_index));state=ready();assert Path(state['project'])==first
        assert simulation()==before,'Changing weapon projects lost the existing motion or pose assignments'
        assert json.loads(second.read_text(encoding='utf-8'))['weapon']==second_record['weapon']
        (work/'result.json').write_text(json.dumps(dict(ok=True,first=str(first),second=str(second),weapons=[first_record['weapon'],second_record['weapon']]),indent=2),encoding='utf-8')
        print('PASS independent ESP/weapon source projects, autosave on switching and exact restored simulation:',work,flush=True)
    finally:
        for name,path in old_paths.items():
            path.write_bytes(old_bytes[name]);command('open_project',dict(path=str(path)),command('revision')['revision']);ready();edit('save_project',dict(path=str(path)));path.write_bytes(old_bytes[name])
        for path,data in destinations.items():
            assert path.resolve().is_relative_to((ROOT/'workspace/projects').resolve())
            if data is None:path.unlink(missing_ok=True)
            else:path.write_bytes(data)
        Path(original).write_bytes(original_bytes);command('open_project',dict(path=original),command('revision')['revision']);ready();edit('save_project',dict(path=original));Path(original).write_bytes(original_bytes)

if __name__=='__main__':run()
