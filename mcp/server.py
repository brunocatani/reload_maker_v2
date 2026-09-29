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
def get_diagnostics() -> dict:
    """Read current source/pose diagnostics without changing the project."""
    return command('diagnostics')

@mcp.tool()
def inspect_pose() -> dict:
    """Read sampled native local and world bone frames at the current time."""
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
def configure_weapon(changes: dict, expected_revision: int) -> dict:
    """Change body male/female, animationSet index, or slot and mod decimal-string IDs."""
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
    """Choose 0 Setup, 1 Preview, 2 Hands/grips, 3 Mechanisms, 4 Reload or 5 Simulate."""
    return command('workspace',{'index':index},expected_revision)

@mcp.tool()
def render_view(view: str = 'current', fit: bool = False, zoom: float | None = None) -> Image:
    """Return a fresh native viewport image: current/front/back/side/top/iso."""
    args={'view':view,'fit':fit}
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
def save_hand_pose(hand: str, name: str, expected_revision: int, notes: str = '') -> dict:
    """Capture the current exact hand and reference placement as a new library pose."""
    return command('save_hand_pose',{'hand':hand,'name':name,'notes':notes},expected_revision)

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
    """Export hand/grip/mechanism/reload/simulation to the local exports folder. Never deploys."""
    return command('export',{'kind':kind},expected_revision)

@mcp.tool()
def get_simulation_state() -> dict:
    """Read direct-operation state, slide source curves, magazine assembly and current part frames."""
    return command('simulation_state')

@mcp.tool()
def simulate_weapon(operation: str, expected_revision: int, progress: float | None = None,
                    cancel: bool = False, enabled: bool = True,
                    position: list[float] | None = None, distance: float | None = None) -> dict:
    """In Simulate: fire/reload/drop_magazine/reset/slide_release, pull (0..1; use 1.08 to retract a locked-open slide), release (cancel), spring (enabled), mag_out/mag_in/grab_magazine/cancel_magazine. move_magazine takes weapon-local offset position[3] while free, or outward distance on the rail. Read ammunition for configured capacity, magazine/chamber counts and lock state. Fresh insertion does not chamber; release or cycle the slide."""
    params={'operation':operation,'cancel':cancel,'enabled':enabled}
    if progress is not None: params['progress']=progress
    if position is not None: params['position']=position
    if distance is not None: params['distance']=distance
    return command('simulate',params,expected_revision)

@mcp.tool()
def cancel_operation() -> dict:
    """Cancel the current source load or reload capture."""
    return command('cancel')

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
