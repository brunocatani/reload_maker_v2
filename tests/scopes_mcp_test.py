"""Real MCP workflow with an optic surface-picked in the running editor.

Opens the L118A2 testbed, whose fitted Schmidt & Bender scope has a glass lens,
and picks the lens itself before the snapshot, so the testbed's saved project
never keeps the picked profile. Export fixtures remain in local/scopes, not in
the user's installable profile folder.
"""
import asyncio
import copy
import json
import os
from pathlib import Path
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'local/scopes'

async def run():
    OUT.mkdir(parents=True, exist_ok=True)
    params = StdioServerParameters(command='py', args=['-3.14', str(ROOT/'mcp/server.py')],
        env=dict(os.environ, TEMP=str(ROOT/'local/tmp'), TMP=str(ROOT/'local/tmp'),
                 PYTHONPYCACHEPREFIX=str(ROOT/'local/pycache')))
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            async def call(tool, **args):
                if args.get('expected_revision') == 'current':
                    args['expected_revision'] = (await call('get_state'))['documentRevision']
                result = await session.call_tool(tool, args)
                if result.isError:
                    raise RuntimeError(str(result.content))
                return json.loads(next(c.text for c in result.content if c.type == 'text'))
            async def op(operation, **values):
                return await call('configure_scope', operation=operation, values=values, expected_revision='current')
            async def ready():
                for _ in range(400):
                    state = await call('get_state')
                    if not state['busy']:
                        return state
                    await asyncio.sleep(.05)
                raise AssertionError('Project load did not complete')
            await ready()
            await call('select_testbed', name='l118a2', expected_revision='current')
            state = await ready()
            await call('export_authored_record', kind='project',path=str(OUT/'initial.project.json'), expected_revision='current')
            await op('open')
            if not (await call('get_scope_state'))['configured']:
                # Scan the viewport for the sight and pick until a lens resolves.
                size = await call('part_at', x=0, y=0)
                picked = False
                for fy in range(20, 81, 3):
                    for fx in range(10, 91, 3):
                        x, y = size['width']*fx/100, size['height']*fy/100
                        if picked or (await call('part_at', x=x, y=y)).get('label') != 'Sight':
                            continue
                        try:
                            await op('pick', x=x, y=y)
                        except RuntimeError:
                            continue
                        scope = await call('get_scope_state')
                        picked = scope['configured'] and scope['resolved']
                assert picked, 'No point on the L118A2 scope resolved a lens'
            baseline = await call('get_scope_state')
            assert baseline['configured'] and baseline['resolved'], 'Pick an optic surface first'
            baseline_pose = await call('inspect_pose')
            export_path = None
            export_before = None
            try:
                changed = copy.deepcopy(baseline['profile'])
                changed['lens'].update(radius=1.4, aspect=.8, eyeRelief=14, cameraForward=11,
                                       rotation=[0, 0, .70710677, .70710677])
                result = await op('edit', profile=changed)
                changed = result['profile']
                assert result['resolved'] and changed['lens']['radius'] > 1.39
                for change in ({'radius': -1}, {'rotation': [0, 0, 0, 0]}, {'position': ['x', 0, 0]}):
                    bad = copy.deepcopy(changed)
                    bad['lens'].update(change)
                    try:
                        await op('edit', profile=bad)
                        raise AssertionError('Malformed profile accepted')
                    except RuntimeError:
                        assert (await call('get_scope_state'))['profile'] == changed
                stale = await session.call_tool('configure_scope', {'operation':'remove', 'expected_revision':0})
                assert stale.isError
                pose = await call('inspect_pose')
                assert all(pose[k] == baseline_pose[k] for k in ('bones','parts','source')), 'Lens edits changed weapon/hand poses'
                saved = OUT/'roundtrip.project.json'
                await call('export_authored_record', kind='project',path=str(saved), expected_revision='current')
                await call('import_project', path=str(saved), expected_revision='current')
                await ready()
                assert (await call('get_scope_state'))['profile'] == changed
                # A different part must not inherit another model's lens.
                other = next(t['index'] for t in baseline['targets'] if t['index'] != baseline['selected'])
                assert not (await op('select', index=other))['configured']
                await op('select', index=baseline['selected'])
                assert (await call('get_scope_state'))['profile'] == changed
                # Snapshot export directory before invoking the real exporter.
                folder = ROOT/'exports/TrueScopesProfiles'
                previous = {p.resolve():p.read_bytes() for p in folder.rglob('*.json')} if folder.exists() else {}
                exported = await op('export')
                assert not exported['deployed']
                export_path = Path(exported['path']).resolve()
                assert folder.resolve() in export_path.parents
                export_before = previous.get(export_path)
                assert json.loads(export_path.read_text(encoding='utf-8')) == changed
                (OUT/'editor-export.json').write_bytes(export_path.read_bytes())
                await op('remove')
                assert not (await call('get_scope_state'))['configured']
                await op('import', path=str(export_path))
                assert (await call('get_scope_state'))['profile'] == changed
                await op('import', path=str(export_path))  # idempotent
                conflict = copy.deepcopy(changed)
                conflict['lens']['radius'] = 2.3
                conflict_path = OUT/'conflict.json'
                conflict_path.write_text(json.dumps(conflict), encoding='utf-8')
                try:
                    await op('import', path=str(conflict_path))
                    raise AssertionError('Conflicting model identity accepted')
                except RuntimeError:
                    assert (await call('get_scope_state'))['profile'] == changed
                malformed = json.loads(saved.read_text(encoding='utf-8'))
                next(iter(malformed['scopeProfiles'].values()))['lens']['radius'] = -1
                bad_project = OUT/'invalid.project.json'
                bad_project.write_text(json.dumps(malformed), encoding='utf-8')
                try:
                    await call('import_project', path=str(bad_project), expected_revision='current')
                    raise AssertionError('Invalid project accepted')
                except RuntimeError:
                    assert (await call('get_scope_state'))['profile'] == changed
                (OUT/'validated.json').write_text(json.dumps({'profile':changed,'export':str(export_path),
                    'checks':['pose isolation','atomic validation','stale revision','project round trip','part isolation',
                              'MO2 export layout','import round trip','duplicate conflict','invalid project rejection']},indent=2),encoding='utf-8')
            finally:
                if export_path:
                    if export_before is None:
                        export_path.unlink(missing_ok=True)
                    else:
                        export_path.write_bytes(export_before)
                await call('import_project', path=str(OUT/'initial.project.json'), expected_revision='current')
                await ready()
            print('PASS real MCP scope edit/save/reopen/export/import, pose isolation and invalid input rejection')

asyncio.run(run())
