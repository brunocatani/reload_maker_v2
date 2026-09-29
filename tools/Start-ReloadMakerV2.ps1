param([string]$Package = '', [switch]$Library, [switch]$Mcp)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$env:TEMP=Join-Path $projectRoot 'local/tmp'
$env:TMP=$env:TEMP
$env:PYTHONPYCACHEPREFIX=Join-Path $projectRoot 'local/pycache'
New-Item -ItemType Directory -Path $env:TEMP -Force | Out-Null
if($Mcp){ & py -3.14 (Join-Path $projectRoot 'mcp/server.py'); exit $LASTEXITCODE }
$executable=Join-Path $projectRoot 'build-fast/dist/ReloadMakerV2/ReloadMakerV2.exe'
if(-not (Test-Path -LiteralPath $executable)){throw 'Build with cmake --preset custom-fast and cmake --build --preset custom-fast first.'}
$arguments=@('--project',('"{0}"' -f $projectRoot))
if($Library){$arguments+='--library'}
if($Package){$arguments+=('"{0}"' -f $Package)}
# The interactive editor window is the application requested by the user.
Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $projectRoot
