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
    library = commands.add_parser('library').add_subparsers(dest='action', required=True)
    library.add_parser('list')
    state = library.add_parser('state')
    state.add_argument('--plugin', required=True)
    commit = library.add_parser('save')
    commit.add_argument('--request', required=True)
    remove = library.add_parser('delete')
    remove.add_argument('--plugin', required=True)
    remove.add_argument('--save', required=True)
    migrate = library.add_parser('migrate')
    migrate.add_argument('--apply', action='store_true', help='Write the ESP projects; without it only report the plan')
    shipping = library.add_parser('export')
    shipping.add_argument('--select', action='append', required=True, help='PLUGIN or PLUGIN@SAVE; the latest version by default')
    shipping.add_argument('--loaded', help='JSON file listing the weapon records of the loaded source; other weapons are skipped')
    shipping.add_argument('--scopes', action='store_true', help='Also export the True Scopes profiles of the exported weapons')
    target = shipping.add_mutually_exclusive_group(required=True)
    target.add_argument('--archive')
    target.add_argument('--mo2')
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
    elif args.command == 'library':
        import product_library
        if args.action == 'list':
            result = dict(ok=True, projects=product_library.list_projects(settings.workspace))
        elif args.action == 'state':
            result = product_library.project_state(settings.workspace, args.plugin)
        elif args.action == 'save':
            result = product_library.save_request(settings.workspace, args.request, backends)
        elif args.action == 'delete':
            result = product_library.delete_save(settings.workspace, args.plugin, args.save)
        elif args.action == 'migrate':
            from product_library_migrate import migrate
            result = migrate(settings.workspace, args.apply)
        else:
            from product_grouped import export_library
            selections = [dict(zip(('plugin', 'save'), value.split('@', 1))) for value in args.select]
            loaded = json.loads(Path(args.loaded).read_text(encoding='utf-8')) if args.loaded else None
            result = asyncio.run(export_library(settings.workspace, selections, backends, args.archive, args.mo2, loaded, args.scopes))
    print(json.dumps(result, allow_nan=False))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(json.dumps(dict(ok=False, error=str(error))))
        sys.exit(1)
