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
    destination = build.add_mutually_exclusive_group()
    destination.add_argument('--archive', help='Complete ZIP or 7z output')
    destination.add_argument('--mo2', help='One destination mod directory inside MO2 mods')
    inspect = packs.add_parser('inspect-project')
    inspect.add_argument('--project', required=True)
    coverage = packs.add_parser('inventory')
    coverage.add_argument('--project', action='append', default=[])
    publish = packs.add_parser('export')
    publish.add_argument('--project', action='append', required=True)
    publish.add_argument('--installation')
    deliver = publish.add_mutually_exclusive_group(required=True)
    deliver.add_argument('--archive')
    deliver.add_argument('--mo2')
    export = packs.add_parser('freeze')
    export.add_argument('--project', required=True)
    export.add_argument('--output', required=True)
    export.add_argument('--skip', action='append', default=[], help='Weapon record left out of this export')
    save = packs.add_parser('save-configuration')
    save.add_argument('--project', required=True)
    save.add_argument('--capture', required=True)
    save.add_argument('--id', default='')
    save.add_argument('--full', default='')
    save.add_argument('--empty', default='')
    batch = packs.add_parser('save-configurations')
    batch.add_argument('--project', required=True)
    batch.add_argument('--captures', required=True, help='Folder of captures, saved in file-name order')
    batch.add_argument('--id', default='')
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
        if args.action == 'inventory':
            from product_coverage import inventory
            result=inventory(settings.workspace,args.project)
        elif args.action == 'validate':
            loaded = [load_pack(Path(p)) for p in args.pack]
            result = dict(ok=True, packs=[dict(id=p['manifest']['id'], objects=len(p['objects'])) for p in loaded],
                          runtimeCatalogValidated=False,
                          message='Pack inputs checked. Building the source-plugin ESLs also validates the complete DIAMOND catalog.')
        elif args.action in ('build', 'export'):
            installation = Path(args.installation or str(Path(settings.workspace)/'workspace/installations/default'))
            initial = args.initialize if args.action == 'build' else not (installation/'current.json').exists()
            inputs = args.pack if args.action == 'build' else args.project
            result = asyncio.run(build_packs(inputs, installation, backends, initial, args.archive, args.mo2))
        elif args.action == 'inspect-project':
            from product_export import inspect_project
            project, weapons = inspect_project(Path(args.project).resolve())
            result = dict(ok=True, id=project['id'], name=project.get('name', project['id']),
                          revision=project['revision'], weapons=weapons, objects=len(project['objects']))
        elif args.action == 'save-configuration':
            from product_authoring import save_configuration
            result=save_configuration(args.project,args.capture,backends,args.id,args.full,args.empty)
        elif args.action == 'save-configurations':
            from product_authoring import save_configurations
            result=save_configurations(args.project,sorted(Path(args.captures).glob('*.json')),backends,args.id)
        else:
            from product_export import export_pack
            result = export_pack(args.project, args.output, backends, args.skip)
    print(json.dumps(result, allow_nan=False))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(json.dumps(dict(ok=False, error=str(error))))
        sys.exit(1)
