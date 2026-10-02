"""Read authored workspace coverage without confusing it with game enablement."""
import os
import json
from pathlib import Path
from product_export import inspect_project,project_source,source_requirements,source_conflicts,form_key
from product_packs import cancellation,digest,read,require


def inventory(workspace,projects=()):
    root=Path(workspace).resolve()/'workspace/packs';candidates={Path(p).resolve() for p in projects}
    require(len(candidates)<=512,'Too many explicit pack projects')
    if root.is_dir():
        visited=0
        for folder,dirs,files in os.walk(root,followlinks=False):
            cancellation();visited+=1;require(visited<=4096,'Workspace pack tree exceeds its directory budget')
            dirs[:]=sorted(d for d in dirs if not d.startswith('.') and d not in ('authoring','objects','Data') and not (Path(folder)/d).is_symlink())
            if 'pack.project.json' in files:candidates.add((Path(folder)/'pack.project.json').resolve())
            require(len(candidates)<=512,'Workspace exceeds 512 authored pack projects')
    result=[]
    for path in sorted(candidates,key=lambda p:str(p).casefold()):
        cancellation();row=dict(project=str(path),status='invalid',weapons=[])
        try:
            before=digest(path);draft=read(path)
            # Preserve recognizable draft ownership even when one referenced
            # input has changed. A damaged draft is not an unconfigured weapon.
            if isinstance(draft,dict) and isinstance(draft.get('weapons'),list) and len(draft['weapons'])<=32:
                for weapon in draft['weapons']:
                    if not isinstance(weapon,dict):continue
                    try:record=form_key(weapon.get('record'))
                    except ValueError:continue
                    row['weapons'].append(dict(record=record,name=str(weapon.get('name',record)),template=str(weapon.get('template',''))))
                row['name']=str(draft.get('name',path.name))
            project,weapons=inspect_project(path)
            conflicts=[]
            for weapon in project['weapons']:
                paths=source_conflicts(source_requirements(weapon,read(project_source(path,weapon['profiles']))))
                if paths:conflicts.append(dict(weapon=weapon['record'],resources=len(paths)))
            require(digest(path)==before,'Pack project changed while coverage was being read')
            row.update(id=project['id'],name=project.get('name',project['id']),revision=project['revision'],sha256=before,
                       status='needs-review' if conflicts else 'saved',weapons=weapons,sourceConflicts=conflicts)
        except (ValueError,OSError,KeyError,TypeError,AttributeError) as error:row['error']=str(error)
        result.append(row)
    authoring=[]
    source_root=Path(workspace).resolve()/'workspace/projects'
    if source_root.is_dir():
        visited=0
        for folder,dirs,files in os.walk(source_root,followlinks=False):
            cancellation();visited+=1;require(visited<=4096,'Authoring project tree exceeds its directory budget')
            dirs[:]=sorted(d for d in dirs if not d.startswith('.') and (Path(folder)/d).resolve().is_relative_to(source_root))
            if 'authoring.project.json' not in files:continue
            require(len(authoring)<512,'Workspace exceeds 512 editor projects')
            path=Path(folder)/'authoring.project.json';row=dict(kind='authoring',project=str(path),status='invalid',weapons=[])
            try:
                before=digest(path);document=read(path)
                require(document.get('format')==1 and document.get('kind')=='reload-maker-v2-project','Invalid editor project')
                weapon=form_key(document.get('weapon'));name=document.get('weaponName')
                if not isinstance(name,str) or not 0<len(name)<=512:name=weapon
                context=document.get('mo2Profile') if document.get('library') else Path(document.get('package','')).name
                require(digest(path)==before,'Editor project changed while reading its entry')
                row.update(name=name,sha256=before,status='draft',context=context,
                           weapons=[dict(record=weapon,name=name)])
            except (ValueError,OSError,KeyError,TypeError,AttributeError) as error:row['error']=str(error)
            authoring.append(row)
    output=dict(ok=True,root=str(root),projects=result,authoringProjects=authoring,scope='authored-workspace',sourceVerification='not-run',gameEnablement='not-checked')
    require(len(json.dumps(output).encode('utf-8'))<=1536*1024,'Workspace coverage exceeds the report budget; inspect individual projects')
    return output
