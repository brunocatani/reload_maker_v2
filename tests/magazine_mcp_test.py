"""Exercise empty/full rendering and free-to-rail insertion through real MCP."""
import asyncio
import base64
import json
import os
from pathlib import Path
from mcp import ClientSession,StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'evidence/magazine'

async def run():
    OUT.mkdir(parents=True,exist_ok=True)
    params=StdioServerParameters(command='py',args=['-3.14',str(ROOT/'mcp/server.py')],
        env=dict(os.environ,TEMP=str(ROOT/'local/tmp'),TMP=str(ROOT/'local/tmp'),PYTHONPYCACHEPREFIX=str(ROOT/'local/pycache')))
    async with stdio_client(params) as (read,write):
        async with ClientSession(read,write) as session:
            await session.initialize()
            async def call(name,**args):
                if args.get('expected_revision')=='current':args['expected_revision']=(await call('get_state'))['revision']
                result=await session.call_tool(name,args)
                assert not result.isError,(name,result)
                text=[c.text for c in result.content if c.type=='text']
                return json.loads(text[0]) if text else result
            async def operation(name,**args):return await call('simulate_weapon',operation=name,expected_revision='current',**args)
            async def ready():
                for _ in range(200):
                    s=await call('get_state')
                    if not s['busy']:
                        assert not s['lastError'],s['lastError']
                        return s
                    await asyncio.sleep(.1)
                raise AssertionError('Loading did not finish')
            async def phase(expected):
                for _ in range(120):
                    s=await call('get_simulation_state')
                    if s['magazine']['phase']==expected:return s
                    await operation('step',seconds=.025)  # commands advance simulated time only by stepping
                raise AssertionError((expected,s['magazine']))
            async def shot(name,view='side'):
                result=await call('render_view',view=view,fit=True,zoom=1.5)
                image=next(c for c in result.content if c.type=='image')
                (OUT/(name+'.png')).write_bytes(base64.b64decode(image.data))
            def visible(s):return [p for p in s['magazineShapes'] if not p['flags']&1]
            def pos(s,track):return next(p['world']['translate'] for p in s['parts'] if p['track']==track)
            await ready();await call('set_workspace',index=5,expected_revision='current');await ready();await operation('reset')
            initial=await call('get_simulation_state');rail=initial['magazineRail']
            assert rail['connectionName']=='P-Mag' and rail['emptyTrack']==3 and rail['fullTrack']==4
            assert rail['magazineMinProjection']+rail['travel']>rail['weaponMaxProjection']
            assert len(visible(initial))==2 and not any(p['emptyVersion'] for p in visible(initial))
            # Empty geometry follows consumed ammunition, not the extraction button.
            for _ in range(initial['ammunition']['capacity']):
                await operation('fire')
                for _ in range(120):
                    if (await call('get_simulation_state'))['action']=='Ready':break
                    await operation('step',seconds=.025)  # commands advance simulated time only by stepping
                else:raise AssertionError('Shot did not complete')
            initial=await call('get_simulation_state')
            assert initial['ammunition']['slideLocked'] and not initial['ammunition']['chambered']
            initial_parts={p['track']:p['world'] for p in initial['parts']}
            await operation('mag_out');await operation('step',seconds=.2)
            moving=await call('get_simulation_state')
            assert moving['magazine']['phase']=='Empty magazine out' and 0<moving['magazine']['distance']<rail['travel']
            empty=await phase('Empty magazine clear')
            assert len(visible(empty))==1 and visible(empty)[0]['emptyVersion']
            for p in empty['parts']:
                if not p['magazineOwner']:assert p['world']==initial_parts[p['track']],'Mag Out moved another weapon part'
            await shot('validated-empty-out','iso')
            await operation('mag_in');await operation('step',seconds=.2)
            incoming=await call('get_simulation_state')
            assert incoming['magazine']['phase']=='Full magazine in' and len(visible(incoming))==2
            await phase('Seated full');await operation('drop_magazine')
            await operation('grab_magazine');await shot('validated-full-magazine','iso')
            r=rail['captureRadius'];axis=rail['outwardInWeapon'];length=rail['travel']
            entry=[v*length for v in axis]
            lateral=[entry[0]+r*5,entry[1],entry[2]]
            free=await operation('move_magazine',position=lateral)
            assert free['magazine']['phase']=='Full magazine at cursor'
            assert len(visible(free))==2 and not any(p['emptyVersion'] for p in visible(free))
            # The cartridge stays with the complete loaded magazine in free space.
            full=pos(free,4);bullet=pos(free,6)
            assert sum((a-b)**2 for a,b in zip(full,bullet))<.01
            guided=await operation('move_magazine',position=entry)
            assert guided['magazine']['phase']=='Full magazine on rail'
            half=await operation('move_magazine',distance=length*.5)
            assert all(abs(a-b*length*.5)<1e-4 for a,b in zip(half['magazine']['offset'],axis))
            await shot('validated-half-in')
            held=await operation('move_magazine',distance=length+rail['releaseMargin']*.5)
            assert held['magazine']['phase']=='Full magazine on rail'
            free=await operation('move_magazine',distance=length+rail['releaseMargin']+1)
            assert free['magazine']['phase']=='Full magazine at cursor'
            await operation('move_magazine',position=entry)
            seated=await operation('move_magazine',distance=0)
            assert seated['magazinePresent'] and seated['magazine']['phase']=='Seated full'
            assert seated['magazine']['offset']==[0,0,0]
            await operation('drop_magazine');await operation('grab_magazine');await operation('cancel_magazine')
            cancelled=await call('get_simulation_state')
            assert cancelled['magazine']['phase']=='Absent' and all(p['flags']&1 for p in cancelled['magazineRoots'])
            await operation('mag_in');await phase('Seated full')
            exported=await call('export_authored_record',kind='simulation',expected_revision='current')
            record=json.loads(Path(exported['path']).read_text(encoding='utf-8'))
            assert record['magazineInteraction']['connectionName']=='P-Mag'
            assert record['recipeId']==record['sourceRecipe']['id']
            # Leaving a cursor-held state must restore native shape flags.
            await operation('drop_magazine');await operation('grab_magazine')
            await call('set_workspace',index=4,expected_revision='current');await ready()
            assert not (await call('get_simulation_state'))['active']
            await call('set_workspace',index=5,expected_revision='current');await ready()
            s=await call('get_simulation_state');assert s['magazinePresent'] and len(visible(s))==2
            project=ROOT/'workspace/Glock19x-simulation.project.json'
            await call('save_project',path=str(project),expected_revision='current')
            saved=json.loads(project.read_text(encoding='utf-8'));assert 'magazineInteraction' in saved
            await call('open_project',path=str(project),expected_revision='current');await ready()
            final=await call('get_simulation_state');assert final['magazinePresent'] and final['magazineRail']['connectionName']=='P-Mag'
            (OUT/'validated-final-state.json').write_text(json.dumps(final,indent=2),encoding='utf-8')
            print('PASS real MCP empty/full variants, complete clearance, animated out/in, free loaded magazine, guided rail, withdrawal hysteresis, seating/cancel, workspace restoration and export/project persistence',flush=True)

if __name__=='__main__':asyncio.run(run())
