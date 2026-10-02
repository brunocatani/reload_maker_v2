param([string]$Package = '', [switch]$Library, [switch]$Mcp)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$env:TEMP=Join-Path $projectRoot 'local/tmp'
$env:TMP=$env:TEMP
$env:PYTHONPYCACHEPREFIX=Join-Path $projectRoot 'local/pycache'
New-Item -ItemType Directory -Path $env:TEMP -Force | Out-Null
$executable=Join-Path $projectRoot 'build-fast/dist/ReloadMakerV2/ReloadMakerV2.exe'
if(-not (Test-Path -LiteralPath $executable)){throw 'Build with cmake --preset custom-fast and cmake --build --preset custom-fast first.'}
if($Mcp){
    $application=Split-Path -Parent $executable
    $python=Join-Path $application 'runtime/python/python.exe'
    if(-not (Test-Path -LiteralPath $python)){throw 'The bundled Python runtime is missing. Rebuild the complete portable application.'}
    & $python -B -I (Join-Path $application 'tools/product_cli.py') --workspace $projectRoot mcp --adapter studio
    exit $LASTEXITCODE
}
$arguments=@('--project',('"{0}"' -f $projectRoot))
if($Library){$arguments+='--library'}
if($Package){$arguments+=('"{0}"' -f $Package)}
# The interactive editor window is the application requested by the user.
Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $projectRoot
