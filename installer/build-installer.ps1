# Builds the Groovebox Windows installer.
#
#   .\installer\build-installer.ps1              refresh launch/ from source, then build the setup
#   .\installer\build-installer.ps1 -SkipBuild   package whatever launch/ already holds
#
# The installer reads its files straight out of launch/, which CMake's `launch` target
# owns, so there is no second copy of the binaries to keep in sync. Output lands in
# launch/ too -- one folder to run and to ship from.
#
# The version defaults to the one declared in CMakeLists.txt rather than a number typed
# into this script, because a setup exe called 0.1.0 that actually holds a newer build is
# worse than no version at all. Pass -Version to override it deliberately.

[CmdletBinding()]
param(
    [switch] $SkipBuild,
    [string] $Configuration = 'Release',
    [string] $Version       = '',
    [string] $BuildDir      = 'build-new'
)

$ErrorActionPreference = 'Stop'

$repoRoot   = Split-Path -Parent $PSScriptRoot
$installer  = $PSScriptRoot
$launch     = Join-Path $repoRoot 'launch'

function Find-Iscc {
    $candidates = @(
        # The compiler that ships inside the Sample Vault project, reached as a sibling
        # under the shared D:\Projects root instead of by absolute path. Absolute paths
        # to 'D:\CURSOR LOCAL PROJECTS\...' stopped resolving when the projects moved.
        (Join-Path $repoRoot '..\..\skeer-audio\sample-vault\installer\InnoSetup6\ISCC.exe'),
        (Join-Path $PSScriptRoot 'InnoSetup6\ISCC.exe'),
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
    )

    foreach ($candidate in $candidates)
    {
        if (Test-Path $candidate)
        {
            return $candidate
        }
    }

    $onPath = Get-Command 'ISCC.exe' -ErrorAction SilentlyContinue

    if ($onPath)
    {
        return $onPath.Source
    }

    throw 'Inno Setup 6 compiler (ISCC.exe) not found. Install it with: winget install JRSoftware.InnoSetup'
}

$iscc = Find-Iscc
Write-Host "compiler: $iscc"

# --- version ----------------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($Version))
{
    $declared = Select-String -Path (Join-Path $repoRoot 'CMakeLists.txt') `
                              -Pattern '^project\s*\([^)]*VERSION\s+(\d+(?:\.\d+){0,3})' `
                              | Select-Object -First 1

    if (-not $declared)
    {
        throw 'Could not read a VERSION from CMakeLists.txt; pass -Version explicitly.'
    }

    $Version = $declared.Matches[0].Groups[1].Value
}

Write-Host "version: $Version"

# --- build -----------------------------------------------------------------
if (-not $SkipBuild)
{
    Write-Host "building $Configuration and refreshing launch/..."
    & cmake --build (Join-Path $repoRoot $BuildDir) --config $Configuration --target launch

    if ($LASTEXITCODE -ne 0)
    {
        throw "build failed with exit code $LASTEXITCODE"
    }
}

# --- verify the deliverables -----------------------------------------------
# Checked before ISCC runs: a missing file would otherwise surface as a compiler error
# about a wildcard that matched nothing, which does not point back at the build.
$required = @(
    (Join-Path $launch 'Groovebox.exe'),
    (Join-Path $launch 'Groovebox.vst3\Contents\x86_64-win\Groovebox.vst3'),
    (Join-Path $launch 'Groovebox.vst3\Contents\Resources\moduleinfo.json'),
    (Join-Path $PSScriptRoot 'README.txt'),
    (Join-Path $PSScriptRoot 'skeer.ico')
)

foreach ($path in $required)
{
    if (-not (Test-Path $path))
    {
        throw "missing deliverable: $path. Run without -SkipBuild first."
    }
}

# --- compile ---------------------------------------------------------------
& $iscc "/DMyAppVersion=$Version" (Join-Path $installer 'Groovebox.iss')

if ($LASTEXITCODE -ne 0)
{
    throw "ISCC failed with exit code $LASTEXITCODE"
}

Write-Host ''
Get-ChildItem $launch -File -Filter '* Setup *.exe' | ForEach-Object {
    Write-Host ("installer: {0}  ({1} MB)" -f $_.FullName, [math]::Round($_.Length / 1MB, 2))
}
