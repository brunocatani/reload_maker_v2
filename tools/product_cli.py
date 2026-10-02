"""Shared headless entry point shipped with Reload Maker."""
import argparse
import asyncio
import json
import os
from pathlib import Path
import runpy
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from product_backend import Backends


def main():
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--workspace', required=True)
    common.add_argument('--application', default=str(Path(__file__).resolve().parents[1]))
    common.add_argument('--cancel-file')
    settings, remaining = common.parse_known_args()
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('self-check')
    automation = commands.add_parser('mcp')
    automation.add_argument('--adapter', choices=['studio', 'nif', 'archive', 'plugin'], default='studio')
    cook = commands.add_parser('cook-object')
    cook.add_argument('--job', required=True)
    cook.add_argument('--output', required=True)
    prepare = commands.add_parser('prepare-object')
    prepare.add_argument('--job', required=True)
    prepare.add_argument('--output', required=True)
    packs = commands.add_parser('packs').add_subparsers(dest='action', required=True)
    validate = packs.add_parser('validate')
    validate.add_argument('--pack', action='append', required=True)
    build = packs.add_parser('build')
    build.add_argument('--pack', action='append', required=True)
    build.add_argument('--installation', required=True)
    build.add_argument('--initialize', action='store_true')
    inspect = packs.add_parser('inspect-project')
    inspect.add_argument('--project', required=True)
    export = packs.add_parser('export')
    export.add_argument('--project', required=True)
    export.add_argument('--output', required=True)
    save = packs.add_parser('save-configuration')
    save.add_argument('--project', required=True)
    save.add_argument('--capture', required=True)
    save.add_argument('--expected', default='')
    save.add_argument('--id', default='')
    save.add_argument('--full', default='')
    save.add_argument('--empty', default='')
    args = parser.parse_args(remaining)
    backends = Backends(Path(settings.application), Path(settings.workspace))
    if settings.cancel_file:
        os.environ['RELOAD_MAKER_CANCEL_FILE'] = str(Path(settings.cancel_file).resolve())
    if args.command == 'self-check':
        result = dict(ok=True, capabilities=backends.capabilities())
    elif args.command == 'cook-object':
        from product_assets import cook_world_object
        result = cook_world_object(args.job, args.output, backends)
    elif args.command == 'prepare-object':
        from product_geometry import prepare_world_object
        result = prepare_world_object(args.job, args.output, backends)
    elif args.command == 'mcp':
        if args.adapter == 'studio':
            sys.argv = [str(backends.application/'mcp/server.py')]
            runpy.run_path(sys.argv[0], run_name='__main__')
        else:
            if args.adapter == 'plugin':
                backends.prepare_serializer()
            backends.adapter(args.adapter).mcp.run()
        return
    else:
        from product_packs import load_pack, build as build_packs
        if args.action == 'validate':
            loaded = [load_pack(Path(p)) for p in args.pack]
            result = dict(ok=True, packs=[dict(id=p['manifest']['id'], objects=len(p['objects'])) for p in loaded])
        elif args.action == 'build':
            result = asyncio.run(build_packs(args.pack, Path(args.installation), backends, args.initialize))
        elif args.action == 'inspect-project':
            from product_export import inspect_project,project_source,source_requirements,source_conflicts
            from product_packs import digest,read
            project_path=Path(args.project).resolve()
            project, weapons = inspect_project(project_path)
            result = dict(ok=True, id=project['id'], name=project.get('name', project['id']),
                          sha256=digest(project_path),revision=project['revision'], weapons=weapons, objects=len(project['objects']))
            result['sourceRequirements']=[dict(weapon=w['record'],configurations=source_requirements(w,read(project_source(project_path,w['profiles'])))) for w in project['weapons']]
            result['sourceConflicts']=[dict(weapon=w['weapon'],paths=source_conflicts(w['configurations'])) for w in result['sourceRequirements'] if source_conflicts(w['configurations'])]
        elif args.action == 'save-configuration':
            from product_authoring import save_configuration
            result=save_configuration(args.project,args.capture,backends,args.expected,args.id,args.full,args.empty)
        else:
            from product_export import export_pack
            result = export_pack(args.project, args.output, backends)
    print(json.dumps(result, allow_nan=False))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(json.dumps(dict(ok=False, error=str(error))))
        sys.exit(1)
