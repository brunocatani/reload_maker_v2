"""One relative tool discovery boundary for GUI jobs, CLI and MCP."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys


class Backends:
    def __init__(self, application: Path, workspace: Path):
        self.application = application.resolve()
        self.workspace = workspace.resolve()
        self.temporary = self.workspace/'local/tmp'
        self.temporary.mkdir(parents=True, exist_ok=True)
        os.environ.update(TEMP=str(self.temporary), TMP=str(self.temporary),
                          DOTNET_ROOT=str(self.application/'runtime/dotnet'),
                          DOTNET_ROOT_X64=str(self.application/'runtime/dotnet'),
                          DOTNET_MULTILEVEL_LOOKUP='0',
                          DOTNET_PROCESSOR_COUNT='2',
                          DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE='true',
                          DOTNET_BUNDLE_EXTRACT_BASE_DIR=str(self.workspace/'local/dotnet-bundles'),
                          DOTNET_CLI_HOME=str(self.workspace/'local/dotnet-home'),
                          DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1',
                          NUGET_PACKAGES=str(self.workspace/'local/nuget'),
                          RELOAD_MAKER_WORKSPACE=str(self.workspace),
                          NIFSKOPE_CLI_EXE=str(self.application/'backends/nif/NifSkope.exe'),
                          BSA_BROWSER_CLI=str(self.application/'backends/archive/bsab.exe'),
                          FO4_MCP_SPRIGGIT_CLI=str(self.application/'backends/plugin/Spriggit.CLI.exe'))
        os.environ['PATH'] = str(self.application/'runtime/dotnet') + os.pathsep + os.environ.get('PATH', '')
        sys.pycache_prefix = str(self.workspace/'local/pycache')
        settings = self.workspace/'local/settings/installation.json'
        os.environ.pop('ELRIC_EXE', None)
        if settings.is_file():
            setup = json.loads(settings.read_text())
            root = Path(setup['gameRoot'] if setup['sameToolsRoot'] else setup['creationKitRoot'])
            for directory in (root/'Tools/Elric', root/'Elric', root):
                if (directory/'Elrich.exe').is_file() and (directory/'Settings/PCMeshes.esf').is_file():
                    os.environ['ELRIC_EXE'] = str(directory/'Elrich.exe')
                    break
        self.loaded = {}

    def adapter(self, name):
        if name not in ('nif', 'plugin', 'archive'):
            raise ValueError('Unsupported native adapter')
        if name not in self.loaded:
            source = self.application/f'backends/adapters/{name}.py'
            spec = importlib.util.spec_from_file_location('reloadmaker_backend_'+name, source)
            module = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = module
            spec.loader.exec_module(module)
            self.loaded[name] = module
        return self.loaded[name]

    def prepare_serializer(self):
        # Spriggit's installed translation is copied into its workspace-local
        # cache. A finished-pack build never downloads missing serializers.
        relative = Path('Translations/Spriggit.Yaml.Fallout4/0.40')
        source = self.application/'backends/plugin'/relative
        if not (self.application/'runtime/dotnet/dotnet.exe').is_file() or not (self.application/'runtime/dotnet/sdk').is_dir():
            raise RuntimeError('Bundled .NET host/serializer discovery SDK is missing; install the complete tool bundle')
        if not (source/'Spriggit.Yaml.Fallout4.exe').is_file():
            raise RuntimeError('Bundled Fallout 4 plugin serializer is incomplete')
        target = self.temporary/'Spriggit'/relative
        shutil.copytree(source, target, dirs_exist_ok=True)

    def capabilities(self):
        paths = dict(nif=self.application/'backends/nif/NifSkope.exe',
                     archive=self.application/'backends/archive/bsab.exe',
                     content=self.application/'backends/diamond/DiamondProfileCheck.exe',
                     plugin=self.application/'backends/plugin/Spriggit.CLI.exe')
        result = {name: dict(available=path.is_file(), path=str(path)) for name, path in paths.items()}
        result['plugin']['available'] &= (self.application/'runtime/dotnet/dotnet.exe').is_file() and (self.application/'runtime/dotnet/sdk').is_dir()
        result['plugin']['available'] &= (self.application/'backends/plugin/Translations/Spriggit.Yaml.Fallout4/0.40/Spriggit.Yaml.Fallout4.exe').is_file()
        return result
