param(
    [ValidateSet('HkxReader', 'Resources', 'Pose', 'SceneBinding', 'Configuration', 'Library', 'HandPose', 'PartGrip', 'GripViewport', 'Mechanism', 'ReloadRecipe', 'Simulation')]
    [string]$Suite = 'HkxReader',
    [string]$QtBin = 'C:\msys64\ucrt64\bin',
    [string]$NifSkopeSource = 'F:\fo4dev\devtools\nifskope-upgraded\nifskope-cli',
    [string[]]$TestArguments = @()
)

$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot -Parent
$taskBuild = Join-Path $taskRepo "build-tests/$Suite"
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null
$env:TEMP=Join-Path $taskRepo 'local/tmp'
$env:TMP=$env:TEMP
$taskOldPath = $env:PATH
try {
    $env:PATH = "$QtBin;C:\msys64\usr\bin;$env:PATH"
    Push-Location $taskBuild
    try {
        & (Join-Path $QtBin 'qmake6.exe') (Join-Path $taskRepo "tests/${Suite}Tests.pro") 'CONFIG+=release' "NIFSKOPE_SOURCE_DIR=$($NifSkopeSource -replace '\\','/')"
        if ($LASTEXITCODE -ne 0) { throw "$Suite test configuration failed" }
        & (Join-Path $QtBin 'mingw32-make.exe') '-j2'
        if ($LASTEXITCODE -ne 0) { throw "$Suite test build failed" }
        & (Join-Path $taskBuild "${Suite}Tests.exe") @TestArguments
        if ($LASTEXITCODE -ne 0) { throw "$Suite tests failed" }
    } finally {
        Pop-Location
    }
} finally {
    $env:PATH = $taskOldPath
}
