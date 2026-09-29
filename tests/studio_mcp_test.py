"""Exercise the real running Glock editor through MCP; all output stays in V2."""
import asyncio
import base64
import json
import math
import os
from pathlib import Path
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT/'evidence/integration'

def qmul(a, b):
    x,y,z,w=a; X,Y,Z,W=b
    return [w*X+x*W+y*Z-z*Y,w*Y-x*Z+y*W+z*X,w*Z+x*Y-y*X+z*W,w*W-x*X-y*Y-z*Z]

async def run():
    EVIDENCE.mkdir(parents=True, exist_ok=True)
    params=StdioServerParameters(command='py', args=['-3.14',str(ROOT/'mcp/server.py')],
        env=dict(os.environ,TEMP=str(ROOT/'local/tmp'),TMP=str(ROOT/'local/tmp'),PYTHONPYCACHEPREFIX=str(ROOT/'local/pycache')))
    async with stdio_client(params) as (read,write):
        async with ClientSession(read,write) as session:
            await session.initialize()
            async def call(tool, **args):
                if args.get('expected_revision')=='current': args['expected_revision']=(await call('get_state'))['revision']
                result=await session.call_tool(tool,args)
                if result.isError: raise AssertionError(f'{tool}: {result}')
                texts=[c.text for c in result.content if c.type=='text']
                return json.loads(texts[0]) if texts else result
            async def idle():
                for _ in range(240):
                    state=await call('get_state')
                    if not state['busy']:
                        assert not state['lastError'], state['lastError']
                        return state
                    await asyncio.sleep(.25)
                raise AssertionError('Load/capture did not complete in 60 seconds')
            async def capture(name, view='iso'):
                result=await call('render_view',view=view,fit=True)
                media=[c for c in result.content if c.type=='image']
                assert media,'MCP did not return native image data'
                path=EVIDENCE/(name+'.png');path.write_bytes(base64.b64decode(media[0].data))
                return path
            state=await idle()
            assert any(w['editorId']=='WPNGlock19x' for w in state['weapons']), 'Launch the editor with the Glock fixture'
            await call('set_workspace',index=1,expected_revision='current')
            state=await call('get_state')
            assert len(state['actions'])>=2 and state['duration']>0
            diag=await call('get_diagnostics')
            assert diag['viewport']['renderer'] and diag['viewport']['fieldOfView']>0
            assert any(m['triangles']>0 and not m['hidden'] for m in diag['viewport']['meshes'])
            await capture('01-native-ready')
            stale=await session.call_tool('select_action',{'index':0,'expected_revision':state['revision']-1})
            assert stale.isError
            await call('restore_hand_animation',hand='left',expected_revision='current')
            await call('restore_hand_animation',hand='right',expected_revision='current')
            await call('control_playback',changes={'playing':False,'seconds':0,'mode':0},expected_revision='current')
            before=await call('inspect_pose')
            bones={b['name']:b for b in before['bones']}
            assert 'Weapon' in bones and 'RArm_Finger13' in bones and 'LArm_Finger13' in bones
            angle=math.radians(3)/2
            delta=[0,0,math.sin(angle),math.cos(angle)]
            right=qmul(bones['RArm_Finger13']['local']['rotation'],delta)
            left=qmul(bones['LArm_Finger13']['local']['rotation'],delta)
            await call('set_workspace',index=2,expected_revision='current')
            await call('edit_finger_joints',hand='right',joints=[{'index':2,'rotation':right}],expected_revision='current')
            after={b['name']:b for b in (await call('inspect_pose'))['bones']}
            for name, b in bones.items():
                if name.startswith('LArm_Finger'): assert after[name]['local']==b['local'], 'Right edit changed left fingers'
            await call('edit_finger_joints',hand='left',joints=[{'index':2,'rotation':left}],expected_revision='current')
            both=await call('inspect_pose'); both_map={b['name']:b for b in both['bones']}
            assert both_map['RArm_Finger13']['local']==after['RArm_Finger13']['local'], 'Left edit lost right draft'
            state=await call('get_state')
            invalid=await session.call_tool('edit_finger_joints',{'hand':'right','joints':[{'index':2,'rotation':[0,0,0,0]}],'expected_revision':state['revision']})
            assert invalid.isError
            assert (await call('get_state'))['revision']==state['revision']
            old_pose_ids={p['id'] for p in (await call('get_state'))['handPoses']}
            state=await call('save_hand_pose',hand='right',name='Glock V2 integration · right',notes='Exact thumb edit and native rest translations',expected_revision='current')
            pose_id=next(p['id'] for p in state['handPoses'] if p['id'] not in old_pose_ids)
            await call('save_hand_pose',hand='left',name='Glock V2 integration · left',expected_revision='current')
            project=ROOT/'workspace/integration.project.json'
            await call('save_project',path=str(project),expected_revision='current')
            await call('open_project',path=str(project),expected_revision='current')
            await idle()
            restored={b['name']:b for b in (await call('inspect_pose'))['bones']}
            for name in ('LArm_Finger13','RArm_Finger13'):
                for a,b in zip(restored[name]['local']['rotation'],both_map[name]['local']['rotation']): assert abs(a-b)<1.e-5
            await capture('02-independent-finger-edits','front')
            await call('start_part_grip',pose_id=pose_id,anchor=0,captured=True,expected_revision='current')
            await call('save_part_grip',expected_revision='current')
            grip_export=await call('export_authored_record',kind='grip',expected_revision='current')
            assert Path(grip_export['path']).is_file() and not grip_export['deployed']
            imported=await call('import_authored_records',paths=[grip_export['path']],expected_revision='current')
            assert imported['imported']==0, 'Identical record should be retained without replacement'
            import uuid
            imported_grip=json.loads(Path(grip_export['path']).read_text(encoding='utf-8'))
            imported_grip['pose']['id']=str(uuid.uuid4())
            imported_grip['pose']['name']='V2 import compatibility check'
            input_file=EVIDENCE/'import-grip.json'
            input_file.write_text(json.dumps(imported_grip,indent=2),encoding='utf-8')
            created=await call('import_authored_records',paths=[str(input_file)],expected_revision='current')
            assert created['imported']==1
            saved_import=Path(created['files'][0])
            assert json.loads(saved_import.read_text(encoding='utf-8'))==imported_grip
            imported_grip['pose']['name']='Conflicting replacement'
            input_file.write_text(json.dumps(imported_grip,indent=2),encoding='utf-8')
            conflict=await session.call_tool('import_authored_records',{'paths':[str(input_file)],'expected_revision':(await call('get_state'))['revision']})
            assert conflict.isError
            assert json.loads(saved_import.read_text(encoding='utf-8'))['pose']['name']=='V2 import compatibility check'
            await call('set_workspace',index=1,expected_revision='current')
            native_again={b['name']:b for b in (await call('inspect_pose'))['bones']}
            for a,b in zip(native_again['RArm_Finger13']['local']['rotation'],bones['RArm_Finger13']['local']['rotation']): assert abs(a-b)<1.e-5
            retained=await call('export_authored_record',kind='grip',expected_revision='current')
            assert retained['path']==grip_export['path'], 'Preview workspace lost the authoring grip'
            state=await call('get_state')
            anchor=next(a['index'] for a in state['anchors'] if a['kind']=='animated-part' and 'WeaponBolt' in a['name'])
            await call('select_mechanism_part',anchor=anchor,expected_revision='current')
            mechanism=await call('control_mechanism',changes={'operation':'set_stage','stage':0,'axis':1,'rotation':False,'from':0,'to':2,'release':'start','returnRate':10,'name':'Draw slide'},expected_revision='current')
            await call('control_mechanism',changes={'operation':'preview'},expected_revision='current')
            await call('control_mechanism',changes={'operation':'seek','path':.5},expected_revision='current')
            await capture('03-mechanism-preview','side')
            await call('control_mechanism',changes={'operation':'release'},expected_revision='current')
            await call('control_mechanism',changes={'operation':'stop'},expected_revision='current')
            await call('control_mechanism',changes={'operation':'save'},expected_revision='current')
            mechanism_export=await call('export_authored_record',kind='mechanism',expected_revision='current')
            assert Path(mechanism_export['path']).is_file()
            await call('learn_reload',expected_revision='current')
            await idle()
            recipe=await call('get_reload_recipe')
            (EVIDENCE/'recipe-before-edit.json').write_text(json.dumps(recipe,indent=2),encoding='utf-8')
            state=await call('get_state'); assert state['recipeWindows']>0 and state['recipeTracks']>0
            await call('edit_reload_window',changes={'window':0,'name':'Inspect native first phase','returnOnRelease':False},expected_revision='current')
            await call('save_reload_recipe',expected_revision='current')
            exported=await call('export_authored_record',kind='reload',expected_revision='current')
            assert Path(exported['path']).is_file()
            native_record=json.loads(Path(exported['path']).read_text(encoding='utf-8'))
            live_record=await call('get_reload_recipe')
            assert live_record['revision']==native_record['revision'], 'Transport overwrote the saved record revision'
            assert live_record['documentRevision']==(await call('get_state'))['revision']
            await capture('04-reload-workspace','iso')
            await call('save_project',path=str(project),expected_revision='current')
            await call('open_project',path=str(project),expected_revision='current')
            final=await idle()
            assert final['recipeWindows']>0
            restored_recipe=await call('get_reload_recipe')
            assert 'Inspect native first phase' in json.dumps(restored_recipe)
            # A source that fails after starting its worker must preserve completed
            # authoring data, not only the old weapon catalog.
            before_failure=await call('get_reload_recipe')
            await call('open_source',path=final['source'],game_data=str(ROOT/'local/missing-data-fixture'),expected_revision='current')
            for _ in range(120):
                failed_state=await call('get_state')
                if not failed_state['busy']: break
                await asyncio.sleep(.1)
            assert not failed_state['busy'] and failed_state['lastError']
            after_failure=await call('get_reload_recipe')
            assert after_failure['id']==before_failure['id'] and after_failure['windows']==before_failure['windows']
            await call('set_workspace',index=4,expected_revision='current')
            final=await call('get_state')
            (EVIDENCE/'final-state.json').write_text(json.dumps(final,indent=2),encoding='utf-8')
            print('PASS: native scene and frames, stale revisions, exact independent finger edits, invalid-edit rejection, hand captures, project reopen, grip export, mechanism preview/release/save, native reload capture/edit/export and reload project reopen',flush=True)

if __name__=='__main__': asyncio.run(run())
