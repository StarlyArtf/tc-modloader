# The viewer's offline half: same shape as simcore/tests/simcore.ps1, and
# deliberately not linked against gdi32 - everything it asserts is in the
# platform independent layer, so it runs anywhere simcore builds.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path (Split-Path (Split-Path $PSScriptRoot))
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskBuild = Join-Path $taskRepo 'build'
New-Item -ItemType Directory -Force $taskBuild | Out-Null
$taskExe = Join-Path $taskBuild 'view-tests.exe'

# The fixture the unknown-kind case reads lives in build/, so it is created
# here rather than committed: it is a deliberately broken board.
@'
# a kind the frozen table does not know
component index=0 kind=0x99 x=0 y=0 rotation=0 word30=1 word40=1
component index=1 kind=0x44 x=6 y=0 rotation=0 word30=1 word40=0
wire index=0 from=2,0 to=5,0 width=1 slot=256
'@ | Set-Content -Encoding ASCII (Join-Path $taskBuild 'unknown-kind.txt')

Push-Location $taskRepo
try {
    & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Werror -static `
        (Join-Path $PSScriptRoot 'view-tests.cpp') -o $taskExe
    if ($LASTEXITCODE) { throw 'the viewer tests did not compile' }
    & $taskExe
    if ($LASTEXITCODE) { throw 'the viewer tests failed' }
} finally {
    Pop-Location
}
"Saved: $taskExe"
