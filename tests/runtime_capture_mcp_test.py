"""Live authoring must produce native runtime records without old importer scripts."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
from template_mcp_test import ready,edit
from server import command

NAMES={'glock':'Glock','ru556':'RU556','m1a':'M1A','timberwolf':'Timberwolf','l118a2':'L118A2',
       'thanatos':'Thanatos',
       'srep-service':'SREPService','srep-ar10':'SREPAR10','srep-honeybadger':'SREPHoneyBadger','srep-xm16e1':'SREPXM16E1'}

# The published SREP AR10, HoneyBadger and XM16E1 packs left out these empty
# bones. Their receivers' meshes hold them under WeaponMagazine, so they exist
# in the game's magazine clone; DIAMOND poses them and nothing visible moves.
# Maker now lists every magazine bone the game has and leaves out only those it
# synthesized from the skeleton, which the clone lacks (DIAMOND would reject them).
EMPTY_RECEIVER_BONES=['WeaponMagazineChild%d'%i for i in range(1,6)]
EXTRA_PARTS={'srep-ar10':EMPTY_RECEIVER_BONES,'srep-honeybadger':EMPTY_RECEIVER_BONES,'srep-xm16e1':EMPTY_RECEIVER_BONES}

def read(path):return json.loads(path.read_text(encoding='utf-8'))
def write(path,value):path.write_text(json.dumps(value,indent=2),encoding='utf-8')

def main(names,editor_project=None):
    if editor_project:assert len(names)==1,'Use one testbed when selecting an explicit preserved editor project'
    work=ROOT/'local/runtime-capture-tests'/uuid.uuid4().hex;work.mkdir(parents=True)
    entries=read(ROOT/'workspace/packs/verified-b5bf9bf910a249da8f180b951aa17ae1/projects.json')
    originals={row['name']:Path(row['path']) for row in entries}
    original=ready()['project'];original_bytes=Path(original).read_bytes();results=[];errors=[]
    try:
        for name in names:
            edit('open_project',{'path':editor_project}) if editor_project else edit('testbed',{'name':name})
            ready();project=Path(ready()['project']);saved=project.read_bytes()
            folder=work/name;folder.mkdir()
            try:
                reference=originals[NAMES[name]];owner=read(reference)['weapons'][0]
                docs={key:read(reference.parent/owner[key]['path']) for key in ('profiles','magazines','sounds','grabs')}
                # Some original Maker projects predate picked points. Use the
                # accepted exported policy in a disposable project for parity.
                fixture=read(project)
                for location in docs['grabs']['locations']:
                    key='|'.join(location[k] for k in ('weapon','group','sourceModel','sourcePath')).replace('\\','/').lower()
                    fixture.setdefault('grabLocations',{}).setdefault(key,location)
                write(folder/'fixture.project.json',fixture)
                edit('open_project',{'path':str(folder/'fixture.project.json')});ready()
                edit('workspace',{'index':5});ready()
                captured=read(Path(edit('export',{'kind':'runtime_configuration'})['path']));write(folder/'capture.json',captured)
                variants=docs['profiles']['profiles'];selector=captured['selection']
                matches=[i for i,v in enumerate(variants) if all(v.get(k,docs['profiles'].get('standardReceiver') if k=='receiver' else None).lower()==value.lower() for k,value in selector.items())]
                assert len(matches)==1,(name,'No unique baseline variant',selector)
                old=copy.deepcopy(variants[matches[0]]);variants[matches[0]]=captured['profile']
                mags=docs['magazines']['magazines'];index=next(i for i,m in enumerate(mags) if m['record'].lower()==selector['magazine'].lower())
                previous=copy.deepcopy(mags[index]);mags[index]=captured['magazine']
                sound_profiles=docs['sounds']['profiles']
                for i,s in enumerate(sound_profiles):
                    if s['magazine'].lower()==selector['magazine'].lower():sound_profiles[i]=captured['sounds']
                for key,value in docs.items():write(folder/(key+'.json'),value)
                exe=ROOT/'build-fast/dist/ReloadMakerV2/backends/diamond/DiamondProfileCheck.exe'
                check=subprocess.run([str(exe),'--authored',*[str(folder/(k+'.json')) for k in ('profiles','magazines','sounds','grabs')]],capture_output=True,text=True)
                assert check.returncode==0,(name,check.stdout,check.stderr)
                # Compare mechanism curves by named follower, preserving their
                # independent complete samples rather than only endpoint travel.
                tracks=lambda p:{t.get('name',t.get('anchor',{}).get('name')):t for t in p['tracks']}
                a,b=tracks(old),tracks(captured['profile']);assert set(a)==set(b),(name,set(a),set(b))
                def close(a,b,path=''):
                    if isinstance(a,(float,int)) and not isinstance(a,bool):assert abs(a-b)<.002,(name,path,a,b)
                    elif isinstance(a,list):
                        assert len(a)==len(b),(name,path,len(a),len(b))
                        for i,(x,y) in enumerate(zip(a,b)):close(x,y,path+'/'+str(i))
                    elif isinstance(a,dict):
                        for k,v in a.items():
                            if k not in ('id','createdAt','sourceRecipeId','name','source','placement','sourcePoseId'):close(v,b[k],path+'/'+k)
                    else:assert a==b,(name,path,a,b)
                for key in a:
                    for field in ('poses','opening','closing','loadedRest','emptyRest','firedRest'):
                        if field in a[key]:close(a[key][field],b[key][field],key+'/'+field)
                for field in ('fireCycle','openingProgress','closingProgress','inputAxis','inputTravel','ejectProgress','operatingFingers'):
                    if field in old:close(old[field],captured['profile'][field],field)
                extra=EXTRA_PARTS.get(name,[])
                assert all(n in [part['name'] for part in captured['magazine']['parts']] for n in extra),(name,'magazine/parts',extra)
                current=dict(captured['magazine'],parts=[part for part in captured['magazine']['parts'] if part['name'] not in extra])
                for field in ('seat','modelInMagazine','motion','parts','emptyParts','grip','rightGrip'):
                    if field in previous:close(previous[field],current[field],'magazine/'+field)
                results.append(dict(testbed=name,selector=selector,nativeAccepted=True));print('PASS',name,flush=True)
            except Exception as error:
                errors.append(dict(testbed=name,error=str(error)));print('FAIL',name,str(error),flush=True)
            finally:
                project.write_bytes(saved);command('open_project',{'path':str(project)},command('revision')['revision']);ready()
                edit('save_project',{'path':str(project)});project.write_bytes(saved)
        write(work/'result.json',dict(ok=not errors,results=results,errors=errors));print('Evidence:',work,flush=True)
        assert not errors,errors
    finally:
        Path(original).write_bytes(original_bytes);command('open_project',{'path':original},command('revision')['revision']);ready()
        edit('save_project',{'path':original});Path(original).write_bytes(original_bytes)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--testbed',action='append');parser.add_argument('--editor-project')
    args=parser.parse_args();main(args.testbed or list(NAMES),args.editor_project)
