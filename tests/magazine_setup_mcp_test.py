"""Explicit magazine pieces/sample settings preserve accepted native presentations."""
import argparse
import json
from pathlib import Path
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit,simulation
from runtime_capture_mcp_test import NAMES,read,write
from server import command

def runtime():return read(Path(edit('export',dict(kind='runtime_configuration'))['path']))

def run(names):
    work=ROOT/'local/magazine-setup-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    index=read(ROOT/'workspace/packs/verified-b5bf9bf910a249da8f180b951aa17ae1/projects.json')
    original=ready()['project'];original_bytes=Path(original).read_bytes();results=[]
    try:
        for name in names:
            edit('testbed',dict(name=name));ready();project=Path(ready()['project']);saved=project.read_bytes()
            folder=work/name;folder.mkdir()
            try:
                reference=Path(next(p['path'] for p in index if p['name']==NAMES[name]));owner=read(reference)['weapons'][0]
                fixture=read(project)
                for location in read(reference.parent/owner['grabs']['path'])['locations']:
                    key='|'.join(location[k] for k in ('weapon','group','sourceModel','sourcePath')).replace('\\','/').lower()
                    fixture.setdefault('grabLocations',{}).setdefault(key,location)
                write(folder/'fixture.project.json',fixture);edit('open_project',dict(path=str(folder/'fixture.project.json')));ready()
                edit('workspace',dict(index=5));ready();before=simulation();profile=runtime();settings=command('magazine_setup_state')['settings']
                bad=dict(settings,recipeId='0'*64)
                try:edit('magazine_setup',dict(operation='save',settings=bad))
                except RuntimeError:pass
                else:raise AssertionError('Stale magazine presentation accepted')
                edit('magazine_setup',dict(operation='save',settings=settings));ready();assert simulation()==before,(name,'Explicit source settings changed simulation')
                after=runtime();after['magazine'].pop('presentationSetup');write(folder/'before.json',profile);write(folder/'after.json',after);assert after==profile,(name,'Explicit source settings changed runtime records')
                bad=dict(settings,pieces=[dict(p,role='spare') for p in settings['pieces']])
                try:edit('magazine_setup',dict(operation='save',settings=bad))
                except RuntimeError:pass
                else:raise AssertionError('Removed every body without error')
                assert command('magazine_setup_state')['settings']==settings
                edit('save_project',dict(path=str(folder/'saved.project.json')));edit('open_project',dict(path=str(folder/'saved.project.json')));ready()
                assert command('magazine_setup_state')['settings']==settings and simulation()==before
                edit('magazine_setup',dict(operation='reset'));ready();assert runtime()==profile
                results.append(name);print('PASS',name,flush=True)
            finally:
                project.write_bytes(saved);command('open_project',dict(path=str(project)),command('state')['revision']);ready()
                edit('save_project',dict(path=str(project)));project.write_bytes(saved)
        write(work/'result.json',dict(ok=True,testbeds=results));print('Evidence:',work,flush=True)
    finally:
        Path(original).write_bytes(original_bytes);command('open_project',dict(path=original),command('state')['revision']);ready()
        edit('save_project',dict(path=original));Path(original).write_bytes(original_bytes)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--testbed',action='append');run(parser.parse_args().testbed or list(NAMES))
