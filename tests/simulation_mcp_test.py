"""Real Glock simulation checks through the running editor's stdio MCP adapter."""
import asyncio
import base64
import json
import os
from pathlib import Path
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT=Path(__file__).resolve().parents[1]
EVIDENCE=ROOT/'evidence/simulation'

async def run():
    EVIDENCE.mkdir(parents=True,exist_ok=True)
    params=StdioServerParameters(command='py',args=['-3.14',str(ROOT/'mcp/server.py')],
        env=dict(os.environ,TEMP=str(ROOT/'local/tmp'),TMP=str(ROOT/'local/tmp'),PYTHONPYCACHEPREFIX=str(ROOT/'local/pycache')))
    async with stdio_client(params) as (read,write):
        async with ClientSession(read,write) as session:
            async def call(name,**args):
                if args.get('expected_revision')=='current':args['expected_revision']=(await call('get_state'))['revision']
                result=await session.call_tool(name,args)
                assert not result.isError,(name,result)
                values=[c.text for c in result.content if c.type=='text']
                return json.loads(values[0]) if values else result
            async def operation(name,**args):return await call('simulate_weapon',operation=name,expected_revision='current',**args)
            async def ready():
                for _ in range(240):
                    s=await call('get_state')
                    if not s['busy']:
                        assert not s['lastError'],s['lastError']
                        return s
                    await asyncio.sleep(.1)
                raise AssertionError('Editor did not become ready')
            async def finished():
                for _ in range(120):
                    s=await call('get_simulation_state')
                    if s['action']=='Ready':return s
                    await asyncio.sleep(.05)
                raise AssertionError('Action did not complete')
            async def capture(name):
                result=await call('render_view',view='side',fit=True,zoom=2)
                media=[c for c in result.content if c.type=='image']
                assert media
                (EVIDENCE/(name+'.png')).write_bytes(base64.b64decode(media[0].data))
            def parts(s):return {p['name']:p['world'] for p in s['parts']}
            def hidden(s):return [x['block'] for x in s['magazineRoots'] if x['flags']&1]
            await session.initialize()
            await ready()
            await call('set_workspace',index=5,expected_revision='current');await ready()
            await operation('reset');await operation('spring',enabled=True)
            initial=await call('get_simulation_state');assert initial['active'] and initial['slide']==0
            initial_parts=parts(initial)
            assert len(initial['setup']['slideTracks'])==4 and len(initial['setup']['magazineTracks'])==7
            recipe_before=await call('get_reload_recipe')
            bones_before=await call('inspect_pose')
            await capture('01-ready')
            pull=await operation('pull',progress=.7)
            assert pull['action']=='Slide held' and abs(pull['slide']-.7)<.001
            pulled=parts(pull)
            assert pulled['WeaponMagazine']==initial_parts['WeaponMagazine'],'Racking advanced the magazine'
            assert pulled['WeaponExtra3']==initial_parts['WeaponExtra3'],'Racking advanced the spare magazine'
            assert pulled['WeaponBolt']['translate'][1]<-2
            assert pulled['WeaponExtra1']['rotate']!=initial_parts['WeaponExtra1']['rotate'],'Barrel did not tilt'
            assert pulled['WeaponMagazineChild2']!=initial_parts['WeaponMagazineChild2'],'Cartridge curve omitted'
            assert (await call('inspect_pose'))['bones']==bones_before['bones'],'Racking changed the source hands'
            await capture('02-linked-pull')
            rejected=await session.call_tool('simulate_weapon',{'operation':'fire','expected_revision':(await call('get_state'))['revision']})
            assert rejected.isError,'Firing accepted an open held slide'
            await operation('release');returned=await finished();assert returned['slide']==0
            removed=await operation('drop_magazine');assert hidden(removed) and not removed['magazinePresent']
            await operation('pull',progress=1);assert hidden(await call('get_simulation_state'))
            await capture('03-dropped-and-racked')
            await operation('release');await finished()
            fire=await operation('fire');assert fire['action']=='Firing' and hidden(fire)
            await asyncio.sleep(.06)
            fire_state=await call('get_state');assert 'fire' in fire_state['animation'].lower()
            await capture('04-fire-without-magazine')
            ended=await finished();assert not ended['magazinePresent'] and hidden(ended)
            reload=await operation('reload');assert reload['action']=='Automatic reload' and reload['magazinePresent'] and not hidden(reload)
            await asyncio.sleep(.8)
            action=await call('get_state');assert 'reload' in action['animation'].lower() and action['seconds']>.5
            await capture('05-automatic-reload')
            ended=await finished();assert ended['magazinePresent'] and ended['slide']==0
            # A mode switch must release simulation visibility and restore the
            # original reload document, including edited phase assignments.
            await operation('drop_magazine')
            await call('set_workspace',index=4,expected_revision='current');await ready()
            assert not (await call('get_simulation_state'))['active']
            recipe_after=await call('get_reload_recipe')
            assert recipe_after['windows']==recipe_before['windows'] and recipe_after['id']==recipe_before['id']
            await call('set_workspace',index=1,expected_revision='current');await ready()
            await call('set_workspace',index=5,expected_revision='current');await ready()
            s=await call('get_simulation_state');assert s['active'] and s['magazinePresent'] and not hidden(s)
            await operation('spring',enabled=False)
            await operation('pull',progress=.6);await operation('release')
            assert abs((await call('get_simulation_state'))['slide']-.6)<.001
            await operation('pull',progress=.9);await operation('release',cancel=True)
            assert abs((await call('get_simulation_state'))['slide']-.6)<.001
            await operation('reset');await operation('spring',enabled=True)
            exported=await call('export_authored_record',kind='simulation',expected_revision='current')
            data=json.loads(Path(exported['path']).read_text(encoding='utf-8'))
            assert data['format']=='reload-maker-simulation' and data['sourceRecipe']['id']==data['recipeId']
            project=ROOT/'workspace/Glock19x-simulation.project.json'
            await call('save_project',path=str(project),expected_revision='current')
            saved=json.loads(project.read_text(encoding='utf-8'))
            assert saved['workspace']==5 and saved['simulationSetup']['recipeId']==data['recipeId']
            await call('open_project',path=str(project),expected_revision='current');await ready()
            final=await call('get_simulation_state');assert final['active'] and final['magazinePresent'] and final['slide']==0
            (EVIDENCE/'mcp-final-state.json').write_text(json.dumps(final,indent=2),encoding='utf-8')
            print('PASS: real MCP direct pull, independent barrel/cartridge curves, unchanged magazine/hands, return/cancel/hold, full magazine removal, fire persistence, automatic reload/reset, workspace restoration and project/export round trip',flush=True)

if __name__=='__main__':asyncio.run(run())
