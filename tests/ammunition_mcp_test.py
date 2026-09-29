"""Validate configured ammunition and both empty-reload routes in the native editor."""
import asyncio
import base64
import json
import os
from pathlib import Path
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'evidence/ammunition'

async def run():
    OUT.mkdir(parents=True, exist_ok=True)
    params = StdioServerParameters(command='py', args=['-3.14', str(ROOT/'mcp/server.py')],
        env=dict(os.environ, TEMP=str(ROOT/'local/tmp'), TMP=str(ROOT/'local/tmp'), PYTHONPYCACHEPREFIX=str(ROOT/'local/pycache')))
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            async def call(name, **args):
                if args.get('expected_revision') == 'current':
                    args['expected_revision'] = (await call('get_state'))['revision']
                result = await session.call_tool(name, args)
                assert not result.isError, (name, result)
                texts = [c.text for c in result.content if c.type == 'text']
                return json.loads(texts[0]) if texts else result
            async def op(name, **args):
                return await call('simulate_weapon', operation=name, expected_revision='current', **args)
            async def ready():
                for _ in range(600):
                    s = await call('get_state')
                    if not s['busy']:
                        assert not s['lastError'], s['lastError']
                        return s
                    await asyncio.sleep(.1)
                raise AssertionError('Editor did not finish loading')
            async def finished():
                for _ in range(400):
                    s = await call('get_simulation_state')
                    if s['action'] == 'Ready' and s['magazine']['phase'] not in ('Full magazine in', 'Empty magazine out', 'Loaded magazine out'):
                        return s
                    await asyncio.sleep(.025)
                raise AssertionError(('Action did not finish', s))
            async def reject(name, **args):
                r = await session.call_tool('simulate_weapon', dict(operation=name, expected_revision=(await call('get_state'))['revision'], **args))
                assert r.isError, ('Invalid action accepted', name)
            async def capture(name, view='side'):
                s = await call('get_simulation_state')
                (OUT/(name+'.json')).write_text(json.dumps(s, indent=2), encoding='utf-8')
                result = await call('render_view', view=view, fit=True, zoom=1.5)
                media = next(c for c in result.content if c.type == 'image')
                (OUT/(name+'.png')).write_bytes(base64.b64decode(media.data))
            def counts(s, mag, chamber, locked):
                a = s['ammunition']
                assert (a['magazineRounds'], a['chambered'], a['slideLocked']) == (mag, chamber, locked), a
            async def empty_weapon(capacity):
                for shot in range(capacity):
                    fired = await op('fire')
                    assert not fired['ammunition']['chambered'] and fired['action'] == 'Firing'
                    ended = await finished()
                    counts(ended, max(0, capacity-shot-2), shot < capacity-1, shot == capacity-1)
                    assert ended['ammunition']['shotsFired'] == shot+1
                assert ended['slide'] == 1 and not ended['ammunition']['canFire']
                await reject('fire')
                return ended
            await ready()
            await call('set_workspace', index=5, expected_revision='current'); await ready()
            await op('reset'); await op('spring', enabled=True)
            state = await call('get_state')
            slot = next(x for x in state['slots'] if x['name'] == 'Magazine')
            original = slot['selected']
            # Rebuild the actual assembly for every supplied magazine variant.
            resolved = []
            for form, capacity in [(0x1000FA4,17),(0x1000FA5,22),(0x100176F,24),(0x1001770,29),(0x1001779,33),(0x100177A,38),(0x1002E6C,50)]:
                current = next(x for x in (await call('get_state'))['slots'] if x['key'] == slot['key'])
                if current['selected'] != str(form):
                    await call('configure_weapon', changes={'slot':slot['key'],'mod':str(form)}, expected_revision='current'); await ready()
                s = await call('get_simulation_state')
                assert s['active'] and s['ammunition']['capacity'] == capacity, s
                assert s['ammunition']['ammoRecord'] == 'Fallout4.esm:01f276' and s['ammunition']['pistolSlideLock']
                counts(s, capacity-1, True, False)
                resolved.append({'form':hex(form),'ammunition':s['ammunition'],'magazineError':s['magazine']['error']})
            (OUT/'configured-capacities.json').write_text(json.dumps(resolved, indent=2), encoding='utf-8')
            await call('configure_weapon', changes={'slot':slot['key'],'mod':str(0x1000FA4)}, expected_revision='current'); await ready()
            await op('reset'); counts(await call('get_simulation_state'),16,True,False)
            await capture('01-ready')
            empty = await empty_weapon(17)
            assert len([p for p in empty['magazineShapes'] if not p['flags']&1]) == 1, 'Empty weapon retained cartridge geometry'
            await capture('02-last-shot-locked')
            await op('mag_out'); removed = await finished()
            assert removed['magazine']['empty'] and not removed['magazinePresent']
            counts(removed,0,False,True); await capture('03-empty-magazine-out','iso')
            await op('grab_magazine')
            rail = (await call('get_simulation_state'))['magazineRail']
            entry = [v*rail['travel'] for v in rail['outwardInWeapon']]
            await op('move_magazine', position=[entry[0]+5*rail['captureRadius'],entry[1],entry[2]])
            await op('move_magazine', position=entry)
            seated = await op('move_magazine', distance=0)
            counts(seated,17,False,True); assert seated['magazinePresent']
            await capture('04-full-magazine-locked'); await reject('fire')
            await op('slide_release'); loaded = await finished()
            counts(loaded,16,True,False); assert loaded['slide'] == 0
            await capture('05-slide-release-chambered')
            await op('pull', progress=.7); await op('release'); counts(await finished(),16,True,False)
            await op('pull', progress=1); await op('release', cancel=True)
            counts(await finished(),16,True,False)
            assert (await call('get_simulation_state'))['ammunition']['roundsEjected'] == 0
            # Physical slingshot after a second complete empty cycle.
            await op('reset'); await empty_weapon(17)
            await op('drop_magazine'); await op('mag_in'); counts(await finished(),17,False,True)
            await op('pull', progress=1); await op('release'); counts(await finished(),17,False,True)
            await op('pull', progress=1.08); await op('release'); counts(await finished(),16,True,False)
            await capture('06-slingshot-chambered')
            # A removed magazine leaves one chambered shot; a full manual rack ejects it.
            await op('drop_magazine'); counts(await call('get_simulation_state'),0,True,False)
            await op('fire'); counts(await finished(),0,False,False); await reject('fire')
            await op('mag_in'); counts(await finished(),17,False,False); await reject('fire')
            await op('pull', progress=1); await op('release'); counts(await finished(),16,True,False)
            await op('drop_magazine'); await op('pull', progress=1); await op('release')
            counts(await finished(),0,False,False); await reject('fire')
            await op('reload'); counts(await finished(),16,True,False)
            exported = await call('export_authored_record', kind='simulation', expected_revision='current')
            data = json.loads(Path(exported['path']).read_text(encoding='utf-8'))
            assert data['ammunitionConfiguration']['capacity'] == 17 and data['ammunitionConfiguration']['pistolSlideLock']
            if original != str(0x1000FA4):
                await call('configure_weapon', changes={'slot':slot['key'],'mod':original}, expected_revision='current'); await ready()
            await op('reset'); await capture('07-final-ready')
            print('PASS real MCP: seven equipped capacities, shot consumption, last-round lock, empty/full magazine visuals, cursor seating without chambering, Slide Release, slingshot, tactical chamber retention, ejection/cancel, dry-fire rejection and automatic reload', flush=True)

if __name__ == '__main__':
    asyncio.run(run())
