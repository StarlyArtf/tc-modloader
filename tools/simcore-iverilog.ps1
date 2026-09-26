# The external judge for the sandbox simulator (docs/PLAN-sandbox-simulator.md
# section 8.2): the same circuits simcore's unit tests run are exported as
# Verilog, simulated with Icarus Verilog, and compared net by net against the
# engine's own event list.  "Our simulator is right" is not a claim this
# repository gets to make on its own.
#
# Icarus Verilog comes from the MSYS2 UCRT64 repository:
#   pacman -S --needed mingw-w64-ucrt-x86_64-iverilog
# When it is not installed the script says so and stops with a skip, so the rest
# of the offline suite still runs on a machine that only has the compiler.
param(
  [switch]$NoBuild,
  [switch]$KeepFiles
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskIverilog = Join-Path $taskCompiler 'iverilog.exe'
$taskVvp = Join-Path $taskCompiler 'vvp.exe'
if (!(Test-Path -LiteralPath $taskIverilog) -or !(Test-Path -LiteralPath $taskVvp)) {
  "SKIP: Icarus Verilog is not installed in $taskCompiler"
  "      install it with: pacman -S --needed mingw-w64-ucrt-x86_64-iverilog"
  exit 0
}

$taskBuild = Join-Path $taskRepo 'build'
$taskWork = Join-Path $taskBuild 'simcore-iverilog'
if (Test-Path -LiteralPath $taskWork) {
  # Only the generated files, and only under build/: the folder is a scratch
  # directory this script owns.
  Remove-Item -LiteralPath $taskWork -Recurse -Force
}
New-Item -ItemType Directory -Force $taskBuild, $taskWork | Out-Null

$taskExe = Join-Path $taskBuild 'simcore-tests.exe'
if (!$NoBuild) {
  & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Werror -static `
      (Join-Path $taskRepo 'simcore\tests\simcore-tests.cpp') -o $taskExe
  if ($LASTEXITCODE) { throw 'simcore tests did not compile' }
}
if (!(Test-Path -LiteralPath $taskExe)) { throw "Missing $taskExe; run without -NoBuild first" }

& $taskExe --emit-verilog $taskWork
if ($LASTEXITCODE) { throw 'Exporting the cross-check circuits failed' }

$taskCompared = 0
Push-Location $taskWork
try {
  foreach ($taskFile in Get-ChildItem -LiteralPath $taskWork -Filter '*.v' | Sort-Object Name) {
    $taskName = [IO.Path]::GetFileNameWithoutExtension($taskFile.Name)
    $taskVvpFile = Join-Path $taskWork ($taskName + '.vvp')
    & $taskIverilog -g2005 -o $taskVvpFile $taskFile.Name
    if ($LASTEXITCODE) { throw "iverilog refused $($taskFile.Name)" }
    & $taskVvp $taskVvpFile
    if ($LASTEXITCODE) { throw "vvp failed on $taskName" }
    $taskVcd = Join-Path $taskWork 'tb.vcd'
    if (!(Test-Path -LiteralPath $taskVcd)) { throw "vvp wrote no VCD for $taskName" }
    $taskExpected = Join-Path $taskWork ($taskName + '.expected')
    $taskSummary = & node (Join-Path $taskRepo 'simcore\tests\vcd-compare.js') $taskVcd $taskExpected
    if ($LASTEXITCODE) { throw "$taskName diverged from iverilog" }
    "PASS $taskName : $taskSummary"
    ++$taskCompared
    Remove-Item -LiteralPath $taskVcd -Force
  }
} finally {
  Pop-Location
}

if (!$taskCompared) { throw 'No cross-check circuit was compared' }
"Compared $taskCompared circuit(s) against $(& $taskIverilog -V | Select-Object -First 1)"
"Work directory: $taskWork"
