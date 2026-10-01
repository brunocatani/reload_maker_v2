"""Local MCP adapter for Reload Maker V2. The interactive application owns state."""
import argparse
import json
import os
from pathlib import Path
from mcp.server.fastmcp import FastMCP, Image

ROOT = Path(__file__).resolve().parents[1]
os.environ['TEMP'] = os.environ['TMP'] = str(ROOT/'local/tmp')
mcp = FastMCP('Reload Maker V2', instructions=(
    'The manually launched native editor must be running. Read get_state before editing and use '
    'its revision. Source import and reload capture are asynchronous: inspect state and continue '
    'when busy is false. Use the configured action list, native meshes, independent physical hands '
    'and actual multi-view renders. Author functional, immersive contacts; shallow clipping is '
    'preferable to hovering. Preserve source packages and keep exports local. Native and '
    'weapon-fixed motion are distinct. Offline preview is not full Havok behavior emulation '
    'or proof of game integration. Save projects and authored records before switching work.'
))

def command(method: str, params: dict | None = None, revision: int | None = None) -> dict:
    request = {'method':method, 'params':params or {}}
    if revision is not None: request['expectedRevision']=revision
    with open(r'\\.\pipe\ReloadMakerV2.local','r+b',buffering=0) as pipe:
        pipe.write(json.dumps(request,allow_nan=False).encode()+b'\n')
        data=bytearray()
        while not data.endswith(b'\n'):
            chunk=pipe.read(65536)
            if not chunk: raise RuntimeError('Editor disconnected before completing the request')
            data.extend(chunk)
            if len(data)>64*1024*1024: raise RuntimeError('Editor response exceeds the supported size')
    response=json.loads(data)
    if not response.get('ok'): raise RuntimeError(response.get('error','Editor rejected the request'))
    return response

@mcp.tool()
def get_state() -> dict:
    """Read sources, configuration, actions, hand poses, parts, revision and busy state."""
    return command('state')

@mcp.tool()
def get_scope_state() -> dict:
    """Read installed optic targets, rigid shape names and current authored lens profile."""
    return command('scope_state')

@mcp.tool()
def configure_scope(operation: str, expected_revision: int, values: dict | None = None) -> dict:
    """Scope operations: open, select(index), pick(x,y viewport pixels), edit(profile), save, import(path), remove, export. Exports remain local."""
    return command('scope_profile', dict(values or {}, operation=operation), expected_revision)

@mcp.tool()
def get_collision_state() -> dict:
    """Read target indices, generated hulls, warnings and asynchronous generation status."""
    return command('collision_state')

@mcp.tool()
def configure_collisions(changes: dict, expected_revision: int) -> dict:
    """Workspace 7: select target index; defaults/generate (gapGame, supportErrorGame, targetPoints); import pre-Elric JSON path against installed model; open saved profile; hull index/enabled; view overlay/compare/isolated; save replaces this weapon/loadout/target profile; export to a local path; cancel worker. Points are source-local game units, not cooked Havok shapes."""
    return command('collisions', changes, expected_revision)

@mcp.tool()
def get_diagnostics() -> dict:
    """Read current source/pose diagnostics without changing the project."""
    return command('diagnostics')

@mcp.tool()
def inspect_pose() -> dict:
    """Read current local/world bone frames and owner-qualified rendered part frames, including active hand and mechanism composition."""
    return command('pose')

@mcp.tool()
def open_source(path: str, game_data: str, expected_revision: int) -> dict:
    """Open a read-only mod folder or ZIP; state reports asynchronous loading."""
    return command('open_source',{'path':path,'gameData':game_data},expected_revision)

@mcp.tool()
def open_installed_library(instance: str, expected_revision: int) -> dict:
    """Read the configured MO2 library; requires the editor to be launched in its virtual context."""
    return command('open_library',{'instance':instance},expected_revision)

@mcp.tool()
def select_weapon(index: int, expected_revision: int) -> dict:
    """Choose a weapon index from the current state and assemble its configuration."""
    return command('select_weapon',{'index':index},expected_revision)

@mcp.tool()
def select_testbed(name: str, expected_revision: int) -> dict:
    """Switch glock/ru556/m1a/timberwolf saved testbeds, retaining the outgoing project. State becomes busy while the source loads."""
    return command('testbed',{'name':name},expected_revision)

@mcp.tool()
def configure_weapon(changes: dict, expected_revision: int) -> dict:
    """Change body male/female, animationSet index, or slot and mod decimal-string IDs. Glock: after selecting the standard magazine and a QuickHands set, reloadNoExtender enables the package's optional unextended-magazine clip. Set it separately from slots/animationSet. Configuration changes rebuild the native interaction source."""
    return command('configure',changes,expected_revision)

@mcp.tool()
def select_action(index: int, expected_revision: int) -> dict:
    """Select a configured animation action from get_state."""
    return command('action',{'index':index},expected_revision)

@mcp.tool()
def control_playback(changes: dict, expected_revision: int) -> dict:
    """Set seconds, playing, loop or mode (0 weapon-fixed, 1 full native)."""
    return command('playback',changes,expected_revision)

@mcp.tool()
def set_workspace(index: int, expected_revision: int) -> dict:
    """Choose 2 Hands/grips, 4 Reload phases, 5 Operate, 6 Sounds, 7 Collisions or 8 Scopes."""
    return command('workspace',{'index':index},expected_revision)

@mcp.tool()
def render_view(view: str = 'current', fit: bool = False, zoom: float | None = None, preserve_camera: bool = False) -> Image:
    """Return a fresh native viewport image: current/front/back/side/top/iso. Fit always references the assembled weapon. preserve_camera restores the user's camera after taking the requested inspection view."""
    args={'view':view,'fit':fit,'preserveCamera':preserve_camera}
    if zoom is not None: args['zoom']=zoom
    result=command('render',args)
    return Image(path=result['path'])

@mcp.tool()
def edit_finger_joints(hand: str, joints: list[dict], expected_revision: int) -> dict:
    """Edit native parent-local joints: index 0-14, rotation [x,y,z,w] unit quaternion."""
    return command('edit_fingers',{'hand':hand,'joints':joints},expected_revision)

@mcp.tool()
def restore_hand_animation(hand: str, expected_revision: int) -> dict:
    """Remove this hand's unsaved finger override and restore native animation; saved records remain intact."""
    return command('restore_hand',{'hand':hand},expected_revision)

@mcp.tool()
def save_hand_pose(hand: str, name: str, expected_revision: int, notes: str = '', anchor: int | None = None) -> dict:
    """Capture the current exact hand and reference placement as a new library pose."""
    params={'hand':hand,'name':name,'notes':notes}
    if anchor is not None: params['anchor']=anchor
    return command('save_hand_pose',params,expected_revision)

@mcp.tool()
def author_reload_grips(expected_revision: int) -> dict:
    """Pause a prepared reload at its current moment and open hand authoring with the weapon fixed and moving magazine selected. Then scrub, choose either hand, and save a pose."""
    return command('author_reload_grips',revision=expected_revision)

@mcp.tool()
def use_for_magazine_grab(expected_revision: int, scope: str = 'magazine') -> dict:
    """Replace this magazine's active fit and save the project. scope=weapon_default sets the current weapon's default, preserving other custom fits. Drums are supported. Old snapshots remain in explicit history; reload_bundle embeds concrete resolved fits."""
    return command('use_magazine_grip',{'scope':scope},expected_revision)

@mcp.tool()
def configure_magazine(expected_revision: int, grip_id: str = '') -> dict:
    """Open the selected magazine in the existing Move/Rotate hand editor. Load its draft/current fit or optionally a specific saved fit from its history. An unassigned magazine offers named Finger poses. Magazine geometry stays authoritative."""
    return command('configure_magazine',{'gripId':grip_id},expected_revision)

@mcp.tool()
def apply_magazine_pose(pose_id: str, expected_revision: int) -> dict:
    """Apply a named Finger pose's exact joints AND saved wrist rotation to the configured magazine. Preserve the current hand position for fine adjustment."""
    return command('apply_magazine_pose',{'poseId':pose_id},expected_revision)

@mcp.tool()
def use_default_magazine_grip(expected_revision: int) -> dict:
    """Remove this magazine's custom assignment, use the weapon default, and save the project."""
    return command('use_default_magazine_grip',revision=expected_revision)

@mcp.tool()
def start_part_grip(pose_id: str, anchor: int, expected_revision: int, captured: bool = True) -> dict:
    """Place a saved hand at a current reference part; choose captured or current placement."""
    return command('start_grip',{'poseId':pose_id,'anchor':anchor,'captured':captured},expected_revision)

@mcp.tool()
def set_part_grip(transform: dict, name: str, expected_revision: int) -> dict:
    """Set hand-in-part transform: rotate[9] native row-major, translate[3], uniform scale."""
    return command('set_grip',{'transform':transform,'name':name},expected_revision)

@mcp.tool()
def save_part_grip(expected_revision: int) -> dict:
    """Save the current validated grip in the local library."""
    return command('save_grip',revision=expected_revision)

@mcp.tool()
def select_mechanism_part(anchor: int, expected_revision: int) -> dict:
    """Select a concrete part to teach or edit its independent mechanism."""
    return command('select_part',{'anchor':anchor},expected_revision)

@mcp.tool()
def control_mechanism(changes: dict, expected_revision: int) -> dict:
    """Read/edit a mechanism; operation read/set_stage/preview/seek/release/stop/save. set_stage takes stage index, axis 0-2, rotation, from/to, pivot, release hold/start/rest and returnRate."""
    return command('mechanism',changes,expected_revision)

@mcp.tool()
def learn_reload(expected_revision: int) -> dict:
    """Capture the configured native reload; inspect busy state before editing its windows."""
    return command('learn_reload',revision=expected_revision)

@mcp.tool()
def get_reload_recipe() -> dict:
    """Read the complete current captured source curves and authored windows."""
    return command('reload')

@mcp.tool()
def edit_reload_window(changes: dict, expected_revision: int) -> dict:
    """Edit window/name/returnOnRelease and track/grabbable/gripId, or split at frame / merge."""
    return command('edit_reload',changes,expected_revision)

@mcp.tool()
def save_reload_recipe(expected_revision: int) -> dict:
    """Save a validated recipe revision to the V2-local library."""
    return command('save_reload',revision=expected_revision)

@mcp.tool()
def save_project(expected_revision: int, path: str = '') -> dict:
    """Save source/configuration/time and current authored drafts atomically inside V2."""
    return command('save_project',{'path':path},expected_revision)

@mcp.tool()
def open_project(path: str, expected_revision: int) -> dict:
    """Validate and reopen an existing project; loading is asynchronous."""
    return command('open_project',{'path':path},expected_revision)

@mcp.tool()
def export_authored_record(kind: str, expected_revision: int) -> dict:
    """Export hand/grip/mechanism/reload/simulation, slide, sounds or reload_bundle to the local exports folder. sounds exports the selected weapon/reload set's saved physical cues and original descriptor identities. slide retains every closing-stroke sample and the native rig paths for DIAMOND. reload_bundle embeds the native recipe, assigned grips, their part definitions and magazine-grab bindings; unresolved references reject export. Never deploys."""
    return command('export',{'kind':kind},expected_revision)

@mcp.tool()
def get_simulation_state() -> dict:
    """Read direct-operation state, slide source curves, magazine assembly and current part frames."""
    return command('simulation_state')

@mcp.tool()
def simulate_weapon(operation: str, expected_revision: int, progress: float | None = None,
                    cancel: bool = False, enabled: bool = True,
                    position: list[float] | None = None, distance: float | None = None,
                    seconds: float | None = None) -> dict:
    """Operate: fire/reload/drop_magazine/reset/slide_release or bolt_release, pull, release(cancel), spring(enabled), mag_out/mag_in/grab_magazine/cancel_magazine. AR-15 uses an independent charging handle (pull 0..1) and bolt catch; trigger_down/up controls held automatic fire. C14 Timberwolf uses pull progress 0..1 to lift/open, then 1..0 to close/lock; release holds position. Firing leaves a spent case until the source ejection threshold; only a full rear-and-forward cycle feeds. step(seconds 0..30) pauses the live clock and advances the same simulation deterministically. move_magazine takes weapon-local offset position[3] while free or outward distance on its straight, weapon-angled input rail. Linear progress drives any sampled magazine rotation/translation; never trace the visual motion path. Fresh insertion does not chamber until the bolt/slide closes."""
    params={'operation':operation,'cancel':cancel,'enabled':enabled}
    if progress is not None: params['progress']=progress
    if position is not None: params['position']=position
    if distance is not None: params['distance']=distance
    if seconds is not None: params['seconds']=seconds
    return command('simulate',params,expected_revision)

@mcp.tool()
def set_interaction_hand(hand: str, expected_revision: int) -> dict:
    """Choose left (right firing grip) or right (left support grip). Glock remains left-only. Finish the current grab first."""
    return command('interaction_hand',{'hand':hand},expected_revision)

@mcp.tool()
def get_grab_locations() -> dict:
    """Read source-local operating grab points, radii and allowed physical hands."""
    return command('grab_locations_state')

@mcp.tool()
def configure_grab_location(operation: str, expected_revision: int, values: dict | None = None) -> dict:
    """open/pick/edit/remove/select/use_palm/save exact operating grab locations. pick takes viewport x/y and optional replace; edit takes id, name, pointInSource[3], radiusGame and handMask (left=1/right=2/both=3). Exports use kind=grab_locations. These are game acquisition locations, independent of wrist/finger poses."""
    return command('grab_locations',dict(values or {},operation=operation),expected_revision)

@mcp.tool()
def get_sound_state() -> dict:
    """Read imported physical cues, original descriptor identities, playback counters and audio errors."""
    return command('sound_state')

@mcp.tool()
def configure_sound(operation: str, expected_revision: int, id: str = '', record: str | None = None,
                    trigger: str | None = None, progress: float | None = None, gain: float | None = None,
                    enabled: bool | None = None) -> dict:
    """Edit, play, stop, save or mute original reload sounds. Movement progress is 0..1; gain 0..2. Save persists this weapon/reload set."""
    params={'operation':operation,'id':id}
    for key,value in {'record':record,'trigger':trigger,'progress':progress,'gain':gain,'enabled':enabled}.items():
        if value is not None:params[key]=value
    return command('sound',params,expected_revision)

@mcp.tool()
def configure_operating_grip(expected_revision: int) -> dict:
    """Open the current operating group's authored hand fit in the existing Move/Rotate editor."""
    return command('configure_operating_grip',{},expected_revision)

@mcp.tool()
def save_operating_grip(expected_revision: int) -> dict:
    """Assign the edited grip to this weapon's input group and physical hand. Replaces that assignment and saves the project."""
    return command('use_operating_grip',{},expected_revision)

@mcp.tool()
def mirror_magazine_grip(expected_revision: int) -> dict:
    """Create an editable opposite-hand magazine fit using the actual skeleton bases. Operating grips remain authored for their native hand."""
    return command('mirror_magazine_grip',{},expected_revision)

@mcp.tool()
def cancel_operation() -> dict:
    """Cancel the current source load or reload capture."""
    return command('cancel')

@mcp.tool()
def export_glock_runtime_slides() -> dict:
    """Capture every available Glock 19X receiver/magazine pair into a native-rig slide pack for DIAMOND. Takes several minutes, restores the current project, and writes only to the local exports folder. Keep the editor idle during capture. Includes the complete sampled slide/barrel/follower stroke; no game deployment."""
    from glock_slide_pack import export_glock_slide_pack
    return export_glock_slide_pack(command)

@mcp.tool()
def import_authored_records(paths: list[str], expected_revision: int) -> dict:
    """Validate and import old or V2 pose/grip/mechanism/recipe JSON. Preserves originals and rejects conflicting IDs."""
    return command('import_records',{'paths':paths},expected_revision)

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--command',help='One JSON request to the local application')
    args=parser.parse_args()
    if args.command:
        request=json.loads(args.command)
        print(json.dumps(command(request['method'],request.get('params'),request.get('expectedRevision'))))
    else: mcp.run()
