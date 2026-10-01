"""Stage pinned local tool installations. No downloads or developer PATH fallback."""
import argparse
import hashlib
import importlib.metadata as metadata
import json
from pathlib import Path
import shutil
import sys
from packaging.requirements import Requirement

ROOT = Path(__file__).resolve().parents[1]


def copy_tree(source, target):
    shutil.copytree(source, target, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '*.pdb'))


def python_runtime(destination):
    source = Path(sys.base_prefix)
    destination.mkdir(parents=True, exist_ok=True)
    for file in source.iterdir():
        if file.is_file() and file.suffix.lower() in ('.exe', '.dll', '.txt'):
            shutil.copy2(file, destination/file.name)
    copy_tree(source/'DLLs', destination/'DLLs')
    shutil.copytree(source/'Lib', destination/'Lib', dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('site-packages', '__pycache__',
                                                 '*.pyc', 'test', 'tests', 'idlelib',
                                                 'ensurepip', 'tkinter', 'turtledemo'))
    installed = destination/'Lib/site-packages'
    installed.mkdir(parents=True, exist_ok=True)
    pending = ['mcp', 'PyYAML']
    packages = {}
    while pending:
        name = pending.pop()
        dist = metadata.distribution(name)
        key = dist.metadata['Name'].lower().replace('_', '-')
        if key in packages:
            continue
        packages[key] = dist.version
        base = Path(dist.locate_file('')).resolve()
        for relative in dist.files or []:
            file = Path(dist.locate_file(relative)).resolve()
            if not file.is_relative_to(base) or not file.is_file() or '__pycache__' in file.parts:
                continue
            target = installed/file.relative_to(base)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(file, target)
        for value in dist.requires or []:
            requirement = Requirement(value)
            if requirement.marker is None or requirement.marker.evaluate({'extra': ''}):
                pending.append(requirement.name)
    # Disable registry, user-site and machine PYTHONPATH discovery.
    (destination/f'python{sys.version_info.major}{sys.version_info.minor}._pth').write_text(
        '.\nLib\nDLLs\nLib/site-packages\n../../tools\nimport site\n')
    return packages


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    config = json.loads(Path(args.config).read_text())
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    packages = python_runtime(out/'runtime/python')
    # The current Spriggit dispatcher invokes `dotnet tool list` even with
    # a complete offline serializer cache. That command needs this SDK in
    # addition to the serializer's runtime; do not rely on a machine SDK.
    dotnet = Path(config['dotnet'])
    net = out/'runtime/dotnet'
    net.mkdir(parents=True, exist_ok=True)
    for name in ('dotnet.exe', 'LICENSE.txt', 'ThirdPartyNotices.txt'):
        shutil.copy2(dotnet/name, net/name)
    for relative in (f'host/fxr/{config["dotnetRuntime"]}',
                     f'shared/Microsoft.NETCore.App/{config["dotnetRuntime"]}',
                     f'sdk/{config["dotnetSdk"]}'):
        copy_tree(dotnet/relative, net/relative)
    copy_tree(Path(config['nif']), out/'backends/nif')
    validator = out/'backends/diamond/DiamondProfileCheck.exe'
    validator.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(Path(config['validator']), validator)
    archive = out/'backends/archive'
    archive.mkdir(parents=True, exist_ok=True)
    for name in ('bsab.exe', 'Sharp.BSA.BA2.dll', 'ICSharpCode.SharpZipLib.dll',
                 'lz4.AnyCPU.loader.dll', 'mw_hash_table.bin'):
        shutil.copy2(Path(config['archive'])/name, archive/name)
    copy_tree(Path(config['archive'])/'Licenses', archive/'Licenses')
    plugin = out/'backends/plugin'
    plugin.mkdir(parents=True, exist_ok=True)
    for name in ('Spriggit.CLI.exe', 'git2-3f4182d.dll'):
        shutil.copy2(Path(config['plugin'])/name, plugin/name)
    copy_tree(Path(config['serializer']), plugin/'Translations/Spriggit.Yaml.Fallout4/0.40')
    copy_tree(ROOT/'backends/adapters', out/'backends/adapters')
    shutil.copy2(ROOT/'backends/provenance.json', out/'backends/provenance.json')
    copy_tree(ROOT/'mcp', out/'mcp')
    copy_tree(ROOT/'skills', out/'skills')
    (out/'tools').mkdir(exist_ok=True)
    for file in (ROOT/'tools').glob('product_*.py'):
        shutil.copy2(file, out/'tools'/file.name)
    for name in ('ReloadMakerTools.cmd', 'ReloadMakerMcp.cmd', 'BuildDiamondPacks.bat'):
        if (ROOT/name).exists():
            shutil.copy2(ROOT/name, out/name)
    files = {}
    for folder in ('runtime', 'backends', 'mcp', 'tools', 'skills'):
        for file in (out/folder).rglob('*'):
            if file.is_file() and '__pycache__' not in file.parts:
                files[file.relative_to(out).as_posix()] = hashlib.file_digest(file.open('rb'), 'sha256').hexdigest()
    manifest = dict(version=1, python=sys.version.split()[0], packages=packages,
                    dotnetRuntime=config['dotnetRuntime'], dotnetSdk=config['dotnetSdk'],
                    sdkReason='Spriggit CLI 0.40.1 invokes dotnet tool list for its offline serializer',
                    serializer='Spriggit.Yaml.Fallout4/0.40', files=files,
                    publicationStatus='Local staging; complete third-party notice/source audit before publication')
    (out/'dependency-manifest.json').write_text(json.dumps(manifest, indent=2))
    print(json.dumps(dict(files=len(files), packages=packages, output=str(out))))


if __name__ == '__main__':
    main()
