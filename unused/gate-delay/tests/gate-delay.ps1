# The offline half of the gate-delay work (docs/PLAN-gate-delay.md): the source
# transform in src/gate_delay.hpp turns an emitted simulation program into a
# unit-stepped one.  The game's own compiler is the final judge, but these cases
# pin down everything that can be checked without launching it.
#
# -Dumps additionally rewrites every native-logic-source-*.txt dump under build/
# (development artifacts, not part of the test's determinism) to shake the
# transform out on whole boards with RAM, ports, probes and registers.
param([switch]$Dumps)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskCompiler = if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskBuild = Join-Path $taskRepo 'build'
New-Item -ItemType Directory -Force $taskBuild | Out-Null
$taskExe = Join-Path $taskBuild 'gate-delay-test.exe'
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Werror -static `
  (Join-Path $PSScriptRoot 'gate-delay.cpp') -o $taskExe
if($LASTEXITCODE){ throw 'Gate delay test did not compile' }
$taskDumps = @()
if ($Dumps) {
    $taskDumps = @(Get-ChildItem -Path (Join-Path $taskBuild '*\native-logic-source-*.txt'), `
                                   (Join-Path $taskBuild 'native-logic-source-*.txt') `
                   -ErrorAction SilentlyContinue |
                   Where-Object { $_.Length -gt 3000 } |
                   Select-Object -First 12 -ExpandProperty FullName)
}
& $taskExe @taskDumps
if($LASTEXITCODE){ throw 'Gate delay test failed' }
