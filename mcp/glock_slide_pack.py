"""Capture every Glock receiver/magazine through the editor's real controller.

The pack contains every sample of each closing stroke, native rig paths and
explicit source-record selections. It never writes to the game installation.
"""
import json
import math
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def export_glock_slide_pack(command, progress=lambda message: None):
    def ready():
        for _ in range(2400):
            state = command('state')
            if not state['busy']:
                if state['lastError']:
                    raise RuntimeError(state['lastError'])
                return state
            time.sleep(.05)
        raise RuntimeError('Editor did not finish the current capture')

    def edit(method, params):
        return command(method, params, ready()['revision'])

    def choose(slot, choice):
        current = next(s for s in ready()['slots'] if s['key'] == slot['key'])
        if current['selected'] != choice['id']:
            edit('configure', {'slot': slot['key'], 'mod': choice['id']})
            ready()

    def capture():
        state = command('simulation_state')
        if not state['active'] or state['error'] or state['magazine']['error']:
            raise RuntimeError('Configured mechanism is not ready: ' + json.dumps(state))
        result = edit('export', {'kind': 'slide'})
        profile = json.loads(Path(result['path']).read_text(encoding='utf-8'))
        if not profile['springReturn'] or profile['frames'] < 2:
            raise RuntimeError('Invalid slide return export')
        expected = profile['sourceLastFrame'] - profile['sourceFirstFrame'] + 1
        if expected != profile['frames']:
            raise RuntimeError('Closing stroke samples were dropped')
        for track in profile['tracks']:
            if not track['runtimePath'].startswith(profile['framePath'] + '/'):
                raise RuntimeError('Part is not bound beneath the live Weapon rig')
            if len(track['poses']) != expected or any(not all(math.isfinite(v) for v in p) for p in track['poses']):
                raise RuntimeError('Invalid source trajectory')
        return profile

    initial = ready()
    all_slots = initial['slots']
    slots = {s['name']: s for s in all_slots if s['name'] in ('Receiver', 'Magazine')}
    if not {'Receiver', 'Magazine'} <= slots.keys():
        raise RuntimeError('Open the Glock 19X package before exporting its slide pack')
    if any('record' not in c for s in all_slots for c in s['choices']):
        raise RuntimeError('Restart the rebuilt editor before exporting native slide bindings')
    original_project = initial['project']
    snapshot = ROOT / 'local/glock-slide-pack-before.project.json'
    edit('save_project', {'path': str(snapshot)})
    profiles = []
    pack = {'format': 'reload-maker-glock-slide-pack', 'version': 1,
            'weapon': 'Glock19x.esp:000f9b', 'profiles': profiles,
            'slots': [{'name': s['name'], 'key': s['key'], 'records': [c['record'] for c in s['choices']]} for s in all_slots]}
    try:
        edit('workspace', {'index': 5})
        auto = next(s['index'] for s in ready()['animationSets'] if s['identity'].get('mode') == 'automatic')
        edit('configure', {'animationSet': auto})
        edit('simulate', {'operation': 'spring', 'enabled': True})
        # Use the same neutral assembly for every pair. The selected slot's
        # actual record/model is still loaded and sampled for all 196 cases.
        neutral = {'Barrel': 'mod_Glock19x_Barrel_Short', 'Damage': 'mod_Glock19x_Damage',
                   'Grip': 'mod_Glock19x_Grip_Standard', 'Muzzle': 'mod_Glock19x_Muzzle',
                   'Sights': 'mod_Glock19x_Sights'}
        for slot in all_slots:
            choice = next((c for c in slot['choices'] if c['editorId'] == neutral.get(slot['name'])
                           or c['editorId'] == 'mod_Glock19x_LowerRail_None'), None)
            if choice:
                choose(slot, choice)
        for receiver in slots['Receiver']['choices']:
            choose(slots['Receiver'], receiver)
            for magazine in slots['Magazine']['choices']:
                choose(slots['Magazine'], magazine)
                profile = capture()
                profile['receiver'] = receiver['record']
                profile['magazine'] = magazine['record']
                if profile['source']['weaponRecord'].lower() != pack['weapon'].lower():
                    raise RuntimeError('The selected weapon is not Glock 19X')
                if any(t['anchor']['record'] not in (receiver['record'], magazine['record']) for t in profile['tracks']):
                    raise RuntimeError('A third configuration slot owns slide motion; export requires an explicit selector')
                profiles.append(profile)
                progress(f"Captured {len(profiles)}: {receiver['name']} / {magazine['name']}")
        # Accessories keep their native animation/attachment ownership. Verify
        # that every admitted non-selector option leaves the driven stroke intact.
        baseline = capture()
        neutral_choices = {s['key']: s['selected'] for s in ready()['slots']}
        verified = 0
        for slot in all_slots:
            if slot['name'] in ('Receiver', 'Magazine'):
                continue
            for choice in slot['choices']:
                choose(slot, choice)
                candidate = capture()
                if len(candidate['tracks']) != len(baseline['tracks']) or candidate['frames'] != baseline['frames']:
                    raise RuntimeError('Accessory changes the driven mechanism: ' + choice['name'])
                for a, b in zip(candidate['tracks'], baseline['tracks']):
                    if a['runtimePath'] != b['runtimePath'] or any(abs(x-y) > .001 for p,q in zip(a['poses'],b['poses']) for x,y in zip(p,q)):
                        raise RuntimeError('Accessory changes the driven trajectory: ' + choice['name'])
                verified += 1
            choose(slot, {'id': neutral_choices[slot['key']]})
        pack['accessoryChecks'] = verified
        destination = ROOT / 'exports/Glock-slide-pack.json'
        staging = destination.with_suffix('.pending')
        staging.write_text(json.dumps(pack, separators=(',', ':'), allow_nan=False), encoding='utf-8')
        staging.replace(destination)
        return {'path': str(destination), 'profiles': len(profiles), 'accessoryChecks': verified, 'deployed': False}
    finally:
        command('open_project', {'path': str(snapshot)}, command('state')['revision'])
        ready()
        if original_project:
            edit('save_project', {'path': original_project})


if __name__ == '__main__':
    from server import command
    print(json.dumps(export_glock_slide_pack(command, lambda message: print(message, flush=True))))
