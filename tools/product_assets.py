"""Cook prepared magazine render models with the configured native asset tools."""
import math
import os
from pathlib import Path
import shutil
import uuid

from product_export import project_source
from product_packs import cancellation, digest, read, require, write


def checked(result, label):
    require(result.get('ok'), label+': '+str(result.get('error', result)))
    return result.get('data')


def render_signature(native, path):
    geometry = checked(native.nif_geometry_stats(str(path)), 'Read render geometry')
    transforms = checked(native.nif_transform_stats(str(path)), 'Read render transforms')['transforms']
    blocks = checked(native.nif_blocks(str(path)), 'Read material bindings')
    require(geometry['shapeCount'] > 0, 'Prepared world model has no render shapes')
    for shape in geometry['shapes']:
        require(not shape['isSkinned'], 'Prepare a rigid world magazine before cooking')
        require(0 < shape['vertices'] <= 65535 and shape['triangles'] > 0, 'Render shape exceeds the Fallout 4 vertex budget')
    return dict(shapes=[{k: v for k, v in shape.items() if k != 'index'} for shape in geometry['shapes']],
                transforms=[{k: v for k, v in item.items() if k != 'index'} for item in transforms],
                materials=sorted((block['type'], block.get('name', '')) for block in blocks
                                 if block['type'] in ('BSLightingShaderProperty', 'BSEffectShaderProperty')))


def equivalent(a, b):
    if type(a) in (int, float) and type(b) in (int, float):return math.isclose(a, b, rel_tol=1e-6, abs_tol=1e-5)
    if isinstance(a, dict) and isinstance(b, dict):return a.keys() == b.keys() and all(equivalent(a[k], b[k]) for k in a)
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):return len(a) == len(b) and all(equivalent(x, y) for x, y in zip(a, b))
    return a == b


def cook_world_object(job_path, output, backends):
    job_path, output = Path(job_path).resolve(), Path(output).resolve()
    job = read(job_path)
    require(job.get('format') == 'reload-maker-world-object-job' and job.get('version') == 1, 'Invalid world-object cook job')
    source, hulls = project_source(job_path, job['geometry']), project_source(job_path, job['collision'])
    require(source.suffix.lower() == '.nif', 'Choose a prepared rigid magazine NIF')
    require(not output.exists() and output.suffix.lower() == '.nif', 'Choose a new finished NIF output')
    collision = read(hulls)
    capture=collision.get('capture',{})
    prepared_report=None;prepared_hash=None
    if capture.get('referencePose',{}).get('space')=='magazine-carrier':
        receipt=source.with_suffix('.prepared.json');require(receipt.is_file(),'Carrier-space geometry needs its preparation receipt')
        prepared_report=read(receipt);prepared_hash=digest(receipt)
        require(prepared_report.get('format')=='reload-maker-prepared-object' and prepared_report.get('modelSha256')==digest(source),
                'Prepared model no longer matches its capture receipt')
        require(prepared_report.get('capture')==capture,'Render and collision differ in magazine, presentation, frame or selected pieces')
    require(isinstance(collision.get('hulls'), list) and 0 < len(collision['hulls']) <= 1024, 'Missing bounded convex hull input')
    count = 0
    for hull in collision['hulls']:
        points = hull.get('points', [])
        require(4 <= len(points) <= 252, 'Each moving-object hull must contain 4–252 support points')
        count += len(points)
        require(all(len(p) == 3 and all(type(v) in (int, float) and math.isfinite(v) and abs(v) < 1e6 for v in p) for p in points), 'Invalid collision support point')
    require(count <= 262144, 'Collision point budget exceeded')
    for name in ('havokScale', 'mass', 'radius'):
        require(type(collision.get(name)) in (int, float) and math.isfinite(collision[name]), 'Missing explicit physical setting: '+name)
    require(collision['havokScale'] == 70 and 0 < collision['mass'] <= 100 and 0 <= collision['radius'] <= .1, 'Unsupported physical settings')
    native = backends.adapter('nif')
    cancellation()
    info = checked(native.nif_info(str(source)), 'Read prepared model')
    require(info['bsVersion'] == 130 and not any(name.startswith('bhk') for name in info['blockTypes']),
            'Prepared input must be a Fallout 4 render model without existing collision')
    checked(native.nif_validate(str(source)), 'Validate prepared model')
    checked(native.nif_check(str(source)), 'Check prepared model')
    before = render_signature(native, source)
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = output.parent/('.cook-'+uuid.uuid4().hex);stage.mkdir()
    try:
        prepared, cooked = stage/'collision.nif', stage/'finished.nif'
        checked(native.nif_add_collision_hulls(str(source), str(hulls), str(prepared)), 'Build bounded native hulls')
        cancellation()
        checked(native.nif_elric_process(str(prepared), str(cooked)), 'Cook Fallout 4 physics')
        cancellation()
        final = checked(native.nif_info(str(cooked)), 'Inspect cooked world model')
        types = final['blockTypes']
        require(final['bsVersion'] == 130 and types.get('bhkNPCollisionObject') == 1 and types.get('bhkPhysicsSystem') == 1,
                'Cooked output lacks its single Fallout 4 dynamic physics body')
        require(not any(name in types for name in ('bhkCollisionObject', 'bhkRigidBody', 'bhkRigidBodyT', 'bhkListShape', 'bhkConvexVerticesShape')),
                'Pre-Elric collision remains in the finished model')
        checked(native.nif_validate(str(cooked)), 'Validate cooked model')
        checked(native.nif_check(str(cooked)), 'Check cooked model')
        after = render_signature(native, cooked)
        require(equivalent(before, after), 'Cooking changed render geometry, transforms or material bindings')
        require(project_source(job_path, job['geometry']) == source and project_source(job_path, job['collision']) == hulls and read(job_path) == job,
                'Asset inputs changed while cooking')
        if prepared_report:require(digest(source.with_suffix('.prepared.json'))==prepared_hash,'Preparation receipt changed while cooking')
        report = dict(format='reload-maker-world-object-receipt', version=1,
            sourceSha256=job['geometry']['sha256'], collisionSha256=job['collision']['sha256'],
            outputSha256=digest(cooked), hulls=len(collision['hulls']), supportPoints=count,
            massKg=collision['mass'], radiusHavok=collision['radius'], render=after,
            engineCooked=True, gameTested=False)
        if prepared_report:report.update(capture=capture,bounds=prepared_report['bounds'])
        cancellation()
        # Both are immutable outputs; a cancelled cook never replaces a model
        # already referenced by an authored pack project.
        receipt = output.with_suffix('.receipt.json')
        require(not receipt.exists(), 'Choose a new output; its receipt already exists')
        write(stage/'receipt.json', report)
        os.replace(stage/'receipt.json', receipt)
        try:os.replace(cooked, output)
        except Exception:
            receipt.unlink();raise
        return dict(ok=True, output=str(output), receipt=str(receipt), hulls=report['hulls'], supportPoints=count, engineCooked=True)
    finally:
        if stage.parent == output.parent and stage.name.startswith('.cook-'):shutil.rmtree(stage)
