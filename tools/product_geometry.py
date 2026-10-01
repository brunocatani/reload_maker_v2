"""Prepare a rigid world model from a frozen native-viewport mesh snapshot.

Only the standard GLB interchange is written here. NIF creation/validation
belongs to the bundled native adapter, using a geometry-free FO4 template.
"""
import json
import math
import os
from pathlib import Path
import shutil
import struct
import uuid

from product_assets import checked, render_signature
from product_packs import cancellation, digest, read, require, write

METRES=.9144/64


def prepare_world_object(snapshot_path, output, backends):
    snapshot_path, output=Path(snapshot_path).resolve(),Path(output).resolve()
    snapshot=read(snapshot_path);source_hash=digest(snapshot_path)
    require(snapshot.get('format')=='reload-maker-visible-part' and snapshot.get('version')==1 and snapshot.get('units')=='game-units','Invalid visible-part snapshot')
    shapes=snapshot.get('meshes',[])
    require(isinstance(shapes,list) and 0<len(shapes)<=128,'Select one bounded rigid magazine/part')
    require(not output.exists() and output.suffix.lower()=='.nif','Choose a new prepared NIF path')
    template=backends.application/'assets/empty-world.nif'
    require(template.is_file(),'The bundled geometry-free world template is missing')
    native=backends.adapter('nif')
    info=checked(native.nif_info(str(template)),'Inspect world template')
    require(info['bsVersion']==130 and info['blockTypes']=={'NiNode':1},'World template contains foreign geometry or physics')
    binary=bytearray();views=[];accessors=[];meshes=[];nodes=[];materials=[]
    vertices=triangles=0

    def attribute(values,width,kind='float'):
        nonlocal binary
        if width==1:values=[[v] for v in values]
        require(values and all(isinstance(row,list) and len(row)==width for row in values),'Invalid mesh attribute width')
        require(all(type(v) in (int,float) and math.isfinite(v) and abs(v)<1e9 for row in values for v in row),'Nonfinite mesh attribute')
        binary+=b'\0'*(-len(binary)%4);offset=len(binary)
        code='f' if kind=='float' else 'H';component=5126 if kind=='float' else 5123
        binary+=struct.pack('<'+code*(len(values)*width),*(v for row in values for v in row))
        views.append(dict(buffer=0,byteOffset=offset,byteLength=len(binary)-offset))
        accessors.append(dict(bufferView=len(views)-1,componentType=component,count=len(values),
                              type={1:'SCALAR',2:'VEC2',3:'VEC3',4:'VEC4'}[width],
                              min=[min(row[i] for row in values) for i in range(width)],
                              max=[max(row[i] for row in values) for i in range(width)]))
        return len(accessors)-1

    for source in shapes:
        cancellation()
        positions=source['positions'];count=len(positions)
        require(0<count<=65535,'A prepared shape exceeds the Fallout 4 vertex limit')
        for key in ('normals','tangents','uvs'):
            require(len(source[key])==count,'Incomplete normals/tangents/UVs in visible geometry')
        indices=source['indices'];require(indices and len(indices)%3==0 and all(type(i) is int and 0<=i<count for i in indices),'Invalid visible triangles')
        vertices+=count;triangles+=len(indices)//3;require(vertices<=500000 and triangles<=1000000,'Prepared object exceeds geometry budget')
        material=source.get('material','');require(isinstance(material,str) and material.lower().endswith(('.bgsm','.bgem')),'Visible shape has no source material')
        attributes=dict(POSITION=attribute([[v*METRES for v in p] for p in positions],3),
                        NORMAL=attribute(source['normals'],3),TANGENT=attribute(source['tangents'],4),
                        TEXCOORD_0=attribute(source['uvs'],2))
        for key,semantic,width in [('uvs2','TEXCOORD_1',2),('colors','COLOR_0',4)]:
            if key in source:
                require(len(source[key])==count,'Incomplete optional vertex attribute');attributes[semantic]=attribute(source[key],width)
        materials.append(dict(name=material));meshes.append(dict(primitives=[dict(attributes=attributes,indices=attribute(indices,1,'ushort'),material=len(materials)-1)]))
        matrix=source['localToRoot'];require(len(matrix)==16 and all(type(v) in (int,float) and math.isfinite(v) for v in matrix),'Invalid captured part transform')
        require(matrix[12:]==[0,0,0,1],'Captured transform must be affine')
        matrix=matrix[:]
        for i in (3,7,11):matrix[i]*=METRES
        nodes.append(dict(name=source['name'],mesh=len(meshes)-1,matrix=[matrix[r*4+c] for c in range(4) for r in range(4)],
                          extras={'Material Path':material,'Flags':14}))
    root=dict(name='MagazineWorld',rotation=[-math.sqrt(.5),0,0,math.sqrt(.5)],children=list(range(1,len(nodes)+1)))
    doc=dict(asset=dict(version='2.0',generator='Reload Maker visible-part capture'),scene=0,scenes=[dict(nodes=[0])],
             nodes=[root,*nodes],meshes=meshes,materials=materials,accessors=accessors,bufferViews=views,buffers=[dict(byteLength=len(binary))])
    raw=json.dumps(doc,separators=(',',':'),allow_nan=False).encode();raw+=b' '*(-len(raw)%4);binary+=b'\0'*(-len(binary)%4)
    output.parent.mkdir(parents=True,exist_ok=True);stage=output.parent/('.prepare-'+uuid.uuid4().hex);stage.mkdir()
    try:
        glb=stage/'visible.glb'
        glb.write_bytes(struct.pack('<III',0x46546c67,2,28+len(raw)+len(binary))+struct.pack('<II',len(raw),0x4e4f534a)+raw+struct.pack('<II',len(binary),0x004e4942)+binary)
        prepared=stage/'prepared.nif'
        checked(native.nif_import_gltf(str(template),str(glb),str(prepared)),'Prepare native magazine geometry')
        checked(native.nif_validate(str(prepared)),'Validate prepared magazine')
        checked(native.nif_check(str(prepared)),'Check prepared magazine')
        signature=render_signature(native,prepared)
        require(len(signature['shapes'])==len(shapes) and sum(s['vertices'] for s in signature['shapes'])==vertices and sum(s['triangles'] for s in signature['shapes'])==triangles,
                'Native import changed geometry coverage')
        transformed=[t for t in signature['transforms'] if t['type'].endswith('Shape')]
        require(len(transformed)==len(shapes),'Prepared transform coverage differs')
        for source,actual,geometry in zip(shapes,transformed,signature['shapes']):
            require(actual['name']==source['name'] and geometry['name']==source['name'],'Native import changed shape identity/order')
            matrix=source['localToRoot']
            require(all(math.isclose(actual['translation'][i],matrix[i*4+3],abs_tol=.0001,rel_tol=1e-6) for i in range(3)) and
                    all(math.isclose(actual['rotation'][r][c]*actual['scale'],matrix[r*4+c],abs_tol=.00001,rel_tol=1e-6) for r in range(3) for c in range(3)),
                    'Prepared model no longer matches the captured magazine frame')
            for bound,select in [('minimum',min),('maximum',max)]:
                require(all(abs(geometry[bound][i]-select(p[i] for p in source['positions']))<=.002 for i in range(3)),
                        'Native vertex bounds differ from the captured source')
            require(geometry['hasNormals'] and geometry['hasTangents'] and geometry['hasUV'],'Native import lost required surface attributes')
        expected=sorted(s['material'].replace('\\','/').lower() for s in shapes)
        require(sorted(name.replace('\\','/').lower() for _,name in signature['materials'])==expected,'Native import changed material references')
        require(digest(snapshot_path)==source_hash,'Visible snapshot changed during preparation')
        cancellation()
        receipt=output.with_suffix('.prepared.json');require(not receipt.exists(),'Prepared receipt already exists')
        write(stage/'receipt.json',dict(format='reload-maker-prepared-object',version=1,snapshotSha256=source_hash,
              modelSha256=digest(prepared),weapon=snapshot['weapon'],target=snapshot['target'],referencePose=snapshot['referencePose'],
              shapes=len(shapes),vertices=vertices,triangles=triangles,materials=expected,engineCooked=False))
        os.replace(stage/'receipt.json',receipt)
        try:os.replace(prepared,output)
        except Exception:
            receipt.unlink();raise
        return dict(ok=True,output=str(output),receipt=str(receipt),shapes=len(shapes),vertices=vertices,triangles=triangles,engineCooked=False,
                    target=snapshot['target'],configuration=snapshot['configuration'],referencePose=snapshot['referencePose'],
                    pieces=[source['source'] for source in shapes])
    finally:
        if stage.parent==output.parent and stage.name.startswith('.prepare-'):shutil.rmtree(stage)
