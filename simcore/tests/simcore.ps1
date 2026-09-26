# The offline half of the sandbox simulator work (docs/PLAN-sandbox-simulator.md
# section 7, S1): simcore is C++ with no game dependency, so its unit tests are
# a plain compile-and-run.  This is the `ctest` the plan asks for - the
# repository has no CMake, and every other offline test here is a PowerShell
# script next to the code it builds, so simcore follows that shape rather than
# adding a second build system.
#
# The external half is tools/simcore-iverilog.ps1: it takes the same circuits
# and asks Icarus Verilog what they should do.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path (Split-Path $PSScriptRoot)
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskBuild = Join-Path $taskRepo 'build'
New-Item -ItemType Directory -Force $taskBuild | Out-Null
$taskExe = Join-Path $taskBuild 'simcore-tests.exe'
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Werror -static `
    (Join-Path $PSScriptRoot 'simcore-tests.cpp') -o $taskExe
if ($LASTEXITCODE) { throw 'simcore tests did not compile' }
& $taskExe
if ($LASTEXITCODE) { throw 'simcore tests failed' }
"Saved: $taskExe"
