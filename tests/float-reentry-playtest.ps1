# True-game case for the Float Ops "leave the sandbox and come back" path
# (examples/float-ops/components.cpp, components_ui.cpp, and the loader's
# level-lifecycle restore in src/native_logic.hpp / src/sim_control.hpp).
#
# The player reports this case exists for (2026-09-26):
#
#   * "常量的值还得我点一下才会刷新" - after an edit (and again after a re-entry)
#     the board kept the value it had computed before;
#   * "进入沙盒后退出再重进，会发现选中元件之后面板的相关配置不会渲染" - after
#     leaving the level and entering it again, selecting a component no longer
#     showed its configuration in the game's own drawer;
#   * "同 2 重进的时候，常量值输出变为默认" - and the Constant came back with the
#     default value instead of the saved one.
#
# The three have one shape: a level that is left and entered again hands the new
# board the same component ids, so "the instance changed" says nothing, and the
# saved configuration is restored on the loader's own time line rather than when
# the player happens to click.  This case runs that whole lifetime in the real
# game:
#
#   * the shipped Mod package (dist\local.float-ops.mod) is the Mod under test;
#   * tests/float-reentry-driver.cpp starts in the level's sandbox (the playtest
#     writes setting_current_level and the enter-board driver opens it), edits one
#     FP32 Constant through the drawer's own `常量值` field with real mouse and
#     keyboard input, leaves the level with the player's own Escape, presses the
#     campaign screen's own level entry again, and then - *without touching a
#     field* - selects the Constant so the drawer opens on it;
#   * what is asserted is the sequence in the log: the typed value reached the
#     record and the circuit was saved, the second board's Constant shows the
#     saved value again before any click, and the drawer read that value back into
#     its own field.
#
# Run it against the shipped package on the previous build and the display stays
# at the default sum (2 instead of 3.5) and the drawer never re-reads the record.
param(
  # Generous: the run has to boot, edit, leave the level, come back and settle.
  [int]$Seconds = 220,
  [string]$Keys = '2.5',
  [string]$Level = 'architecture',
  # The red check: run the case against a loader and a Mod package built before a
  # fix (the packed files the previous round shipped) instead of this tree's.
  [string]$Loader = '',
  [string]$Mod = '',
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskCxx = Join-Path $taskCompiler 'g++.exe'
$taskSandbox = Join-Path $taskRepo ('build\float-reentry-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskSandbox 'game'
$taskProfile = Join-Path $taskSandbox 'home'
$taskProfileDir = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskDriverId = 'test.float-reentry'
$taskMods = @('local.float-ops', $taskDriverId)
$taskLevel = $Level
$taskSchemaPaths = @(
  (Join-Path $taskProfileDir ('schematics\' + $taskLevel + '\Default\circuit.data'))
)

foreach ($taskRequired in @($taskCxx, (Join-Path $taskRepo 'dist\tc-loader.dll'),
                             (Join-Path $taskRepo 'dist\tcmod-cli.exe'),
                             (Join-Path $taskGame 'Turing Complete.exe'))) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}

# The Mod under test is the package the player installs.
if (!$Mod) {
  & (Join-Path $taskRepo 'examples\float-ops\build.ps1') -SkipTests | Out-Host
  if ($LASTEXITCODE) { throw 'Float Ops build failed' }
}

# The board: the M2 fixture (two FP32 Constants -> FP32 Add -> FP32 Display),
# injected as the saved circuit of the level's own Default branch - the board a
# sandbox edit shows.
New-Item -ItemType Directory -Force $taskRoot, $taskProfile,
  (Join-Path $taskRoot 'mods') | Out-Null
foreach ($taskSchema in $taskSchemaPaths) {
  New-Item -ItemType Directory -Force (Split-Path -Parent $taskSchema) | Out-Null
}
$taskFixtureExe = Join-Path $taskSandbox 'float-fixture.exe'
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static (Join-Path $PSScriptRoot 'float-fixture.cpp') `
  -o $taskFixtureExe
if ($LASTEXITCODE) { throw 'The fixture generator did not compile' }
$taskBoard = Join-Path $taskSandbox 'float-reentry-board.data'
& $taskFixtureExe '--m2' $taskBoard | Out-Host
if ($LASTEXITCODE -or !(Test-Path -LiteralPath $taskBoard)) {
  throw 'The M2 fixture was not generated'
}
foreach ($taskSchema in $taskSchemaPaths) {
  Copy-Item -LiteralPath $taskBoard -Destination $taskSchema -Force
}

# A small windowed client: the driver clicks by client coordinates, so the window
# has to fit the desktop the sandbox runs on.
@(
  'setting_window_mode = true'
  'setting_window_position = 0'
  'setting_window_size = 52430000'
  'setting_language = Chinese (Simplified)'
  ('setting_current_level = ' + $taskLevel)
) | Set-Content -LiteralPath (Join-Path $taskProfileDir 'settings.txt') -Encoding ascii

foreach ($taskDir in @('asset', 'campaign', 'translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll', 'soft_oal.dll',
                         'steam_api64.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
$taskLoaderPath = if ($Loader) { (Resolve-Path -LiteralPath $Loader).Path }
                  else { Join-Path $taskRepo 'dist\tc-loader.dll' }
Copy-Item -LiteralPath $taskLoaderPath -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
$taskModPath = if ($Mod) { (Resolve-Path -LiteralPath $Mod).Path }
               else { Join-Path $taskRepo 'dist\local.float-ops.mod' }
Copy-Item -LiteralPath $taskModPath -Destination (Join-Path $taskRoot 'mods\local.float-ops.mod') -Force

# The driver package: test-only, never part of a shipped Mod.
$taskDriverPackage = Join-Path $taskSandbox 'driver-package'
New-Item -ItemType Directory -Force (Join-Path $taskDriverPackage 'native') | Out-Null
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static -shared -I (Join-Path $taskRepo 'sdk') `
  (Join-Path $PSScriptRoot 'float-reentry-driver.cpp') -o (Join-Path $taskDriverPackage 'native\driver.dll')
if ($LASTEXITCODE) { throw 'The re-entry driver did not compile' }
("{`"format`":2,`"id`":`"$taskDriverId`",`"name`":`"Float Ops re-entry driver`",`"version`":`"0.1.0`"," +
 "`"capabilities`":[`"log`",`"events`",`"services`",`"game_handles`"]," +
 "`"native`":{`"api`":1,`"entry`":`"native/driver.dll`"}}") |
  Set-Content -LiteralPath (Join-Path $taskDriverPackage 'mod.json') -Encoding ascii
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskDriverPackage `
  -Output (Join-Path $taskRoot ('mods\' + $taskDriverId + '.mod')) | Out-Null
if ($LASTEXITCODE) { throw 'The re-entry driver package was not written' }

& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskMods | Out-Host
if ($LASTEXITCODE) { throw 'The sandbox Mod apply failed' }

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousLayout = $env:TC_FLOATOPS_LAYOUT
$taskPreviousBodies = $env:TC_FLOATOPS_BODIES
$taskPreviousKeys = $env:TC_FLOAT_REENTRY_KEYS
$taskPreviousLevel = $env:TC_FLOAT_REENTRY_LEVEL

# The game decides for itself which board it opens at startup, and a launch that
# never showed the fixture board says nothing about the Mod, so the launch is
# retried before the assertions run.
$taskAttempts = 3
for ($taskAttempt = 1; $taskAttempt -le $taskAttempts; ++$taskAttempt) {
  $taskProcess = $null
  try {
    $env:USERPROFILE = $taskProfile
    $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
    # The Mod's per-instance layout line, which is where the value it shows on the
    # board is read back from.
    $env:TC_FLOATOPS_LAYOUT = '1'
    # The Mod publishes every body's rectangle so the driver can click the
    # component the way the player does: that click is what opens the drawer.
    $env:TC_FLOATOPS_BODIES = '1'
    $env:TC_FLOAT_REENTRY_KEYS = $Keys
    $env:TC_FLOAT_REENTRY_LEVEL = $taskLevel
    # Every attempt starts from the same board: an attempt that reached the save
    # would otherwise hand the next one a board that already carries the edit,
    # which is a different case (and a different failure message).
    foreach ($taskSchema in $taskSchemaPaths) {
      Copy-Item -LiteralPath $taskBoard -Destination $taskSchema -Force
    }
    Remove-Item -LiteralPath $taskLog -ErrorAction SilentlyContinue
    # A visible window, because the driver posts real mouse messages and has to
    # find the game's window to post them to.
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
      -WorkingDirectory $taskRoot -PassThru
    $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
      Start-Sleep -Milliseconds 500
      $taskAttemptText = if (Test-Path -LiteralPath $taskLog) {
        Get-Content -LiteralPath $taskLog -Raw -Encoding UTF8
      } else { '' }
      if ($taskAttemptText.Contains('DRIVER: done') -or
          $taskAttemptText.Contains('DRIVER: giving up') -or
          $taskProcess.HasExited) { break }
    } while ([DateTime]::UtcNow -lt $taskDeadline)
  } finally {
    $env:USERPROFILE = $taskPreviousProfile
    $env:APPDATA = $taskPreviousAppData
    $env:TC_FLOATOPS_LAYOUT = $taskPreviousLayout
    $env:TC_FLOATOPS_BODIES = $taskPreviousBodies
    $env:TC_FLOAT_REENTRY_KEYS = $taskPreviousKeys
    $env:TC_FLOAT_REENTRY_LEVEL = $taskPreviousLevel
    if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
      [void]$taskProcess.CloseMainWindow()
      if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
    }
  }
  $taskAttemptLog = if (Test-Path -LiteralPath $taskLog) {
    Get-Content -LiteralPath $taskLog -Raw -Encoding UTF8
  } else { '' }
  if ($taskAttemptLog.Contains('DRIVER: done')) { break }
  "attempt $taskAttempt did not reach the end (the fixture board may not have come up); retrying"
}

if (!(Test-Path -LiteralPath $taskLog)) { throw "No loader log; inspect $taskSandbox" }
# The loader log is UTF-8 without a BOM: Windows PowerShell would decode it with
# the ANSI code page and mangle the non-ASCII text this case also asserts on.
$taskLines = Get-Content -LiteralPath $taskLog -Encoding UTF8
$taskText = $taskLines -join "`n"
$taskEvidence = @($taskLines | Where-Object {
  $_ -match 'float-ops|DRIVER:|Component panel|Level loaded|scene'
})
$taskEvidence | Select-Object -Last 80 | ForEach-Object { $_ }

function Assert-TaskContains([string]$Needle) {
  if (!$taskText.Contains($Needle)) {
    throw "Missing evidence: $Needle (inspect $taskLog)"
  }
}

function Get-TaskIndex([string]$Pattern, [int]$Start = 0) {
  for ($taskIndex = $Start; $taskIndex -lt $taskLines.Count; ++$taskIndex) {
    if ($taskLines[$taskIndex] -match $Pattern) { return $taskIndex }
  }
  return -1
}

# ---- the edit, on the first visit -------------------------------------------

Assert-TaskContains 'DRIVER: board A is up; the target instance is 0x'
Assert-TaskContains 'DRIVER: clicked the component body (before leaving)'
Assert-TaskContains 'DRIVER: panel rows before leaving: instance 0x'
Assert-TaskContains 'DRIVER: clicked the value field'
Assert-TaskContains ('DRIVER: typed "' + $Keys + '" and pressed Enter')

# The typed text has to reach the instance's own record: that is the write the
# player makes, and the record is what the second visit has to come back with.
# (The field already holds the record's text - "1" for a fresh Constant - so the
# typing lands after it; what matters is that the edit *moved* the value and that
# both visits agree, not which decimal the typing produced.)
$taskWrittenBits = -1
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'float-ops: constant 0x([0-9a-f]+) set to 0x([0-9a-f]+)') {
    $taskWrittenBits = [Convert]::ToUInt32($Matches[2], 16)
  }
}
if ($taskWrittenBits -lt 0) { throw "The Constant's value was never written; inspect $taskLog" }
if ($taskWrittenBits -eq 0x3F800000) {
  throw "The Constant still holds the default 1.0 after the edit; inspect $taskLog"
}
Assert-TaskContains 'float-ops: component configuration saved in the game''s circuit file'
if ($taskText.Contains('did not complete the circuit save') -or
    $taskText.Contains('no current Board handle was available to save')) {
  throw "The circuit save failed; the second visit cannot see the saved value (inspect $taskLog)"
}

# What the first visit ended on: the value the board showed and the text the
# drawer's own field held.  The second visit has to come back with exactly these
# two, which is the player's report in one line ("the Constant goes back to the
# default after a re-entry" / "the panel no longer shows its configuration").
$taskLeave = Get-TaskIndex 'DRIVER: trying the game''s own way out'
if ($taskLeave -lt 0) { throw "The driver never left the level; inspect $taskLog" }
$taskBeforeValue = ''
$taskBeforeBits = 0
for ($taskIndex = $taskLeave - 1; $taskIndex -ge 0; --$taskIndex) {
  if ($taskLines[$taskIndex] -match 'float-ops M2: display 0x[0-9a-f]+ shows (.+) 0x([0-9a-f]{8}) \(bits=0x([0-9a-f]{8})\)') {
    $taskBeforeValue = $Matches[1]
    $taskBeforeBits = $Matches[3]
    break
  }
}
if (!$taskBeforeValue) { throw "No display value before the level was left; inspect $taskLog" }
$taskFieldText = ''
for ($taskIndex = $taskLeave - 1; $taskIndex -ge 0; --$taskIndex) {
  if ($taskLines[$taskIndex] -match 'float-ops: the drawer (?:is editing|re-read) instance 0x[0-9a-f]+ \(type 0x[0-9a-f]+, label "[^"]*", value "([^"]*)"\)') {
    $taskFieldText = $Matches[1]
    break
  }
}
# A package that predates the field report in that line still has to be checked,
# so the text is only compared when the Mod actually writes it.
$taskValueText = ''
if ($taskFieldText) {
  if ($taskFieldText -eq '1') {
    throw "The drawer's field never showed the edited value before leaving; inspect $taskLog"
  }
  $taskValueText = ([double][single]::Parse($taskFieldText, [System.Globalization.CultureInfo]::InvariantCulture)).ToString(
    [System.Globalization.CultureInfo]::InvariantCulture)
}

# ---- the level lifetime ------------------------------------------------------

Assert-TaskContains 'DRIVER: trying the game''s own way out (Escape)'
Assert-TaskContains 'DRIVER: left the level by itself; the old board handle is gone'
Assert-TaskContains 'DRIVER: pressing the level entry (button #'
Assert-TaskContains 'DRIVER: pressed the level entry (button #'
Assert-TaskContains 'DRIVER: board B is up (level loads='

$taskLoads = @($taskLines | Where-Object { $_ -match '^Level loaded: ' })
if ($taskLoads.Count -lt 2) {
  throw "The level was loaded $($taskLoads.Count) time(s); this case needs two visits (inspect $taskLog)"
}
# The second visit starts with the driver's second entry press: the game loads a
# level more than once per entry, so counting "Level loaded" lines would put the
# window inside the first visit.
$taskReentry = Get-TaskIndex 'DRIVER: pressing the level entry.*to reach the level again'
if ($taskReentry -lt 0) { throw "The driver never went back for the level; inspect $taskLog" }
if ((Get-TaskIndex 'restored \d+ configuration byte\(s\) from its saved record' $taskReentry) -lt 0) {
  throw "The saved record was already restored before the second visit; inspect $taskLog"
}

# ---- what the second visit has to show by itself -----------------------------

# (1) The Constant publishes its saved value again without anyone clicking: the
#     display is fed by it, and the line proving it has to arrive after the
#     second level load and before the driver selects anything.
$taskDisplayPattern = 'float-ops M2: display 0x[0-9a-f]+ shows .*0x' + $taskBeforeBits
$taskDisplay = Get-TaskIndex $taskDisplayPattern $taskReentry
if ($taskDisplay -lt 0) {
  throw "The Constant did not come back with the saved value (nothing showed 0x$taskBeforeBits again after the re-entry); inspect $taskLog"
}
$taskSelectAfter = Get-TaskIndex 'DRIVER: board B settled; selecting 0x'
if ($taskSelectAfter -lt 0 -or $taskSelectAfter -lt $taskDisplay) {
  throw "The saved value only appeared after the driver selected the component; inspect $taskLog"
}
# ... and it has to be the value the board *ends* on: the default sum appearing
# again after the refresh would be the same bug in another order.
$taskLastDisplay = -1
for ($taskIndex = $taskLines.Count - 1; $taskIndex -gt $taskReentry; --$taskIndex) {
  if ($taskLines[$taskIndex] -match 'float-ops M2: display 0x[0-9a-f]+ shows ') {
    $taskLastDisplay = $taskIndex
    break
  }
}
if ($taskLastDisplay -lt 0 -or $taskLines[$taskLastDisplay] -notmatch $taskDisplayPattern) {
  throw "The board's last displayed value is not the saved one; inspect $taskLog"
}

# (2) The drawer renders for that component after the re-entry, and its rows were
#     read from the *new* board's record - the panel file it writes carries the
#     frame the rows were painted for.
Assert-TaskContains 'DRIVER: clicked the component body (after re-entry)'
Assert-TaskContains 'DRIVER: panel rows after re-entry: instance 0x'

$taskFrames = @()
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'DRIVER: panel rows (before leaving|after re-entry): instance 0x([0-9a-f]+) type 0x([0-9a-f]+) frame=(\d+) rows=(\d+) \[(.*)\]') {
    $taskFrames += [pscustomobject]@{
      When = $Matches[1]; Instance = $Matches[2]; Type = $Matches[3]
      Frame = [int]$Matches[4]; Rows = [int]$Matches[5]; Kinds = $Matches[6]
    }
  }
}
$taskBefore = $taskFrames | Where-Object { $_.When -eq 'before leaving' } | Select-Object -Last 1
$taskAfter = $taskFrames | Where-Object { $_.When -eq 'after re-entry' } | Select-Object -Last 1
if (!$taskBefore -or !$taskAfter) { throw "The drawer's rows were not reported on both visits (inspect $taskLog)" }
if ($taskAfter.Instance -ne $taskBefore.Instance) {
  throw "The second visit's drawer showed a different instance; inspect $taskLog"
}
if ($taskAfter.Frame -le $taskBefore.Frame) {
  throw "The drawer's rows were not painted again after the re-entry (frame $($taskAfter.Frame) <= $($taskBefore.Frame)); inspect $taskLog"
}
if ($taskAfter.Kinds -notmatch 'value') {
  throw "The Constant's value row was missing after the re-entry (rows [$($taskAfter.Kinds)]); inspect $taskLog"
}

# (3) The rows were filled from the record, not from the board that is gone: the
#     Mod's own line names the instance, its type and the text now in the fields,
#     and it has to appear after the second level load with the value the first
#     visit ended on.
$taskDrawerPattern = 'float-ops: the drawer (?:is editing|re-read) instance 0x' + $taskAfter.Instance +
  ' \(type 0x' + $taskAfter.Type
if ($taskValueText) { $taskDrawerPattern += ', label "[^"]*", value "' + [regex]::Escape($taskValueText) + '"\)' }
if ((Get-TaskIndex $taskDrawerPattern $taskReentry) -lt 0) {
  throw "The drawer never read the re-entered board's record back into its fields; inspect $taskLog"
}

# A field the driver never touched after the re-entry: the saved value reached the
# panel by itself.
$taskFieldClicks = @($taskLines | Where-Object { $_ -match 'DRIVER: clicked the value field' })
if ($taskFieldClicks.Count -ne 1) {
  throw "The value field was clicked $($taskFieldClicks.Count) time(s); the re-entry must not need a click (inspect $taskLog)"
}

# A write that could not find its instance is the player's "写入元件配置失败".
foreach ($taskDiagnostic in @('no live handle for instance',
                              'the host refused the configuration for instance',
                              'this write was not attempted because')) {
  if ($taskText.Contains($taskDiagnostic)) {
    throw "A configuration write failed ($taskDiagnostic); inspect $taskLog"
  }
}

Assert-TaskContains 'DRIVER: done'

"PASS float-ops re-entry: the edit was written and saved, the level was left and entered again, and the second visit showed the saved value on the board and in the drawer's own field without a click"
