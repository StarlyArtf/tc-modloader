# True-game case for the Float Ops editor rows in the game's own component
# drawer (the fields are declared in examples/float-ops/editor.cpp; the loader
# draws and commits them).
#
# The player's report this case exists for: "after selecting a float component,
# the input box and the checkbox in the bottom panel do nothing".  Everything
# the case needs is the real game, a real board and real input:
#
#   * the shipped Mod package (dist\local.float-ops.mod) is the Mod under test,
#     not a test build of it;
#   * the board is the M2 fixture (two FP32 Constants -> FP32 Add -> FP32
#     Display) placed in the sandbox level of an isolated profile;
#   * tests/float-panel-driver.cpp selects the FP32 Constant through the board's
#     own selection call (the game opens its drawer on it), walks the real cursor
#     onto the drawer's `标签` field and clicks it with real mouse messages, then
#     types with real keyboard messages and presses Enter;
#   * the Mod reports what its own rows saw (`float-ops panel trace:`), so the
#     log can tell whether the game's panel delivered the input to the plugin's
#     own widget.
#
# Note on the sandbox: the game does not always come up with the fixture board
# loaded (its own startup decides which board it opens), so a red run is not by
# itself a Mod failure - the log says which line is missing, and the case says
# `stage=0` in the driver's heartbeats when the board never appeared.
#
# Two independent facts have to agree for the case to pass: a row widget that
# ImGui reported as hovered/clicked, and a label the game then shows on the
# component (`render: … name="xy"`), which is the player-visible outcome.
#
# `-Fixture crowd` runs the same case on a board with 22 native components and
# the M2 chain *behind* the crowd.  That is the shape of the next player report
# ("所有能写配置的元件都写入不了", 2026-09-26): the host's instance enumeration
# answers OK only when every instance fitted the caller's buffer, so a Mod with
# a fixed eight-handle array refuses every write on a board that carries more
# native parts than that.  With the chain behind eighteen unwired displays the
# edited constant is far past the old buffer, so this run fails unless the Mod
# grows its buffer to the total the host reports
# (tests/float-fixture.cpp --crowd).
param(
  [int]$Seconds = 150,
  [string]$Select = 'constant',
  [string]$Keys = 'xy',
  [ValidateSet('m2', 'crowd')][string]$Fixture = 'm2',
  # A board file to use instead of a generated fixture (a player's own saved
  # board, which is how the "every write fails" report was reproduced against a
  # real 22-component schematic).
  [string]$Board = '',
  # The board is placed in the level's own schematic branch, which is the one the
  # enter-board driver opens here (measured; the fixture has to be somewhere the
  # game actually looks).
  [string]$Level = 'architecture',
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskCxx = Join-Path $taskCompiler 'g++.exe'
$taskBoardName = if ($Board) { 'saved' } else { $Fixture }
$taskSandbox = Join-Path $taskRepo ('build\float-panel-' + $taskBoardName + '-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskSandbox 'game'
$taskProfile = Join-Path $taskSandbox 'home'
$taskProfileDir = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskDriverId = 'test.float-panel'
$taskMods = @('dev.enter-board', 'local.float-ops', $taskDriverId)
# The enter-board driver opens the level's *sandbox* edit, and that is the board
# the game shows here: measured, the drawer and its editable rows only exist for
# it, while the level's own branch loads the board in "test" mode.  So the
# fixture goes to the sandbox schematic path and the level's own branch is left
# alone.
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
& (Join-Path $taskRepo 'examples\float-ops\build.ps1') -SkipTests | Out-Host
if ($LASTEXITCODE) { throw 'Float Ops build failed' }

# The board: the M2 fixture, injected as the saved board of the level the
# enter-board driver opens (the same recipe as tests/sandbox-sim-playtest.ps1,
# which is the case that knows how to reach a board with no input at all).
New-Item -ItemType Directory -Force $taskRoot, $taskProfile,
  (Join-Path $taskRoot 'mods') | Out-Null
foreach ($taskSchema in $taskSchemaPaths) {
  New-Item -ItemType Directory -Force (Split-Path -Parent $taskSchema) | Out-Null
}
$taskFixtureExe = Join-Path $taskSandbox 'float-fixture.exe'
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static (Join-Path $PSScriptRoot 'float-fixture.cpp') `
  -o $taskFixtureExe
if ($LASTEXITCODE) { throw 'The fixture generator did not compile' }
$taskBoard = Join-Path $taskSandbox ('float-' + $taskBoardName + '-board.data')
if ($Board) {
  if (!(Test-Path -LiteralPath $Board)) { throw "No such board file: $Board" }
  Copy-Item -LiteralPath $Board -Destination $taskBoard -Force
  "using the saved board $Board"
} else {
  & $taskFixtureExe ('--' + $Fixture) $taskBoard | Out-Host
  if ($LASTEXITCODE -or !(Test-Path -LiteralPath $taskBoard)) {
    throw "The $Fixture fixture was not generated"
  }
}
foreach ($taskSchema in $taskSchemaPaths) {
  Copy-Item -LiteralPath $taskBoard -Destination $taskSchema -Force
}

# A small windowed client: the driver clicks by client coordinates, so the
# window has to fit the desktop the sandbox runs on.
@(
  'setting_window_mode = true'
  'setting_window_position = 0'
  'setting_window_size = 52430000'
  'setting_language = Chinese (Simplified)'
  # The sandbox edit is the one the drawer exists in, and the empty level name is
  # what makes the game open it.
  ('setting_current_level = ' + $taskLevel)
) | Set-Content -LiteralPath (Join-Path $taskProfileDir 'settings.txt') -Encoding ascii

foreach ($taskDir in @('asset', 'campaign', 'translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll', 'soft_oal.dll',
                         'steam_api64.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
foreach ($taskMod in @('dev.enter-board', 'local.float-ops')) {
  Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\' + $taskMod + '.mod')) -Destination (Join-Path $taskRoot 'mods') -Force
}

# The driver package: test-only, never part of a shipped Mod.
$taskDriverPackage = Join-Path $taskSandbox 'driver-package'
New-Item -ItemType Directory -Force (Join-Path $taskDriverPackage 'native') | Out-Null
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static -shared -I (Join-Path $taskRepo 'sdk') `
  (Join-Path $PSScriptRoot 'float-panel-driver.cpp') -o (Join-Path $taskDriverPackage 'native\driver.dll')
if ($LASTEXITCODE) { throw 'The panel driver did not compile' }
("{`"format`":2,`"id`":`"$taskDriverId`",`"name`":`"Float Ops panel input driver`",`"version`":`"0.1.0`"," +
 "`"native`":{`"api`":1,`"entry`":`"native/driver.dll`"}}") |
  Set-Content -LiteralPath (Join-Path $taskDriverPackage 'mod.json') -Encoding ascii
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskDriverPackage `
  -Output (Join-Path $taskRoot ('mods\' + $taskDriverId + '.mod')) | Out-Null
if ($LASTEXITCODE) { throw 'The panel driver package was not written' }

& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskMods | Out-Host
if ($LASTEXITCODE) { throw 'The sandbox Mod apply failed' }

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousUi = $env:TC_FLOATOPS_UI
$taskPreviousLayout = $env:TC_FLOATOPS_LAYOUT
$taskPreviousFallback = $env:TC_FLOATOPS_FALLBACK
$taskPreviousDriverFallback = $env:TC_FLOAT_FALLBACK
$taskPreviousBodies = $env:TC_FLOATOPS_BODIES
$taskPreviousSelect = $env:TC_FLOAT_SELECT
$taskPreviousKeys = $env:TC_FLOAT_KEYS
$taskPreviousRows = $env:TC_FLOAT_ROWS
$taskPreviousTrace = $env:TC_EDITOR_TRACE
# The game decides for itself which board it opens at startup, and a launch that
# never showed the fixture board says nothing about the Mod, so the launch is
# retried before the assertions run.
$taskAttempts = 3
for ($taskAttempt = 1; $taskAttempt -le $taskAttempts; ++$taskAttempt) {
  $taskProcess = $null
  try {
    $env:USERPROFILE = $taskProfile
    $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
    # The rows are the loader's now (tc.component.editor), so the driver reads
    # their rectangles from the loader's own trace instead of a file this Mod
    # used to write.  TC_FLOAT_ROWS carries the log path the driver parses.
    $env:TC_FLOATOPS_UI = '1'
    $env:TC_EDITOR_TRACE = '1'
    $env:TC_FLOAT_ROWS = $taskLog
    # The Mod's per-instance layout line, which is where the label it shows on the
    # board is read back from.
    $env:TC_FLOATOPS_LAYOUT = '1'
    # The Mod publishes every body's rectangle so the driver can click the
    # component the way the player does: that click is what opens the drawer.
    $env:TC_FLOATOPS_BODIES = '1'
    $env:TC_FLOAT_SELECT = $Select
    $env:TC_FLOAT_KEYS = $Keys
    Remove-Item -LiteralPath $taskLog -ErrorAction SilentlyContinue
    # A visible window, because the driver posts real mouse messages and has to
    # find the game's window to post them to.
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
      -WorkingDirectory $taskRoot -PassThru
    $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
      Start-Sleep -Milliseconds 500
      $taskAttemptText = if (Test-Path -LiteralPath $taskLog) { Get-Content -LiteralPath $taskLog -Raw } else { '' }
      if ($taskAttemptText.Contains('DRIVER: done') -or
          $taskAttemptText.Contains('DRIVER: the Mod never reported') -or
          $taskProcess.HasExited) { break }
    } while ([DateTime]::UtcNow -lt $taskDeadline)
  } finally {
    $env:USERPROFILE = $taskPreviousProfile
    $env:APPDATA = $taskPreviousAppData
    $env:TC_FLOATOPS_UI = $taskPreviousUi
    $env:TC_FLOATOPS_LAYOUT = $taskPreviousLayout
    $env:TC_FLOATOPS_FALLBACK = $taskPreviousFallback
    $env:TC_FLOAT_FALLBACK = $taskPreviousDriverFallback
    $env:TC_FLOATOPS_BODIES = $taskPreviousBodies
    $env:TC_FLOAT_SELECT = $taskPreviousSelect
    $env:TC_FLOAT_KEYS = $taskPreviousKeys
    $env:TC_FLOAT_ROWS = $taskPreviousRows
    $env:TC_EDITOR_TRACE = $taskPreviousTrace
    if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
      [void]$taskProcess.CloseMainWindow()
      if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
    }
  }
  $taskAttemptLog = if (Test-Path -LiteralPath $taskLog) { Get-Content -LiteralPath $taskLog -Raw } else { '' }
  if ($taskAttemptLog.Contains('DRIVER: done')) { break }
  "attempt $taskAttempt did not reach the end (the fixture board may not have come up); retrying"
}

if (!(Test-Path -LiteralPath $taskLog)) { throw "No loader log; inspect $taskSandbox" }
$taskLines = Get-Content -LiteralPath $taskLog
$taskText = $taskLines -join "`n"
$taskEvidence = @($taskLines | Where-Object {
  $_ -match 'float-ops|DRIVER:|Component panel'
})
$taskEvidence | Select-Object -Last 60 | ForEach-Object { $_ }

function Assert-TaskContains([string]$Needle) {
  if (!$taskText.Contains($Needle)) {
    throw "Missing evidence: $Needle (inspect $taskLog)"
  }
}

Assert-TaskContains 'float-ops: 65 type(s) declared their property rows to the loader (tc.component.editor V2)'
Assert-TaskContains 'DRIVER: the target instance is'
Assert-TaskContains 'Component editor: window instance=0x'
Assert-TaskContains 'Component editor: row instance=0x'
Assert-TaskContains 'DRIVER: hovering the label field'
Assert-TaskContains 'DRIVER: clicked the label field'
Assert-TaskContains ('DRIVER: typed "' + $Keys + '" and pressed Enter')
Assert-TaskContains 'DRIVER: done'
# The bottom panel's own art for a Mod component is hidden with the foundry button
# (both say "this type has no schematic behind it"), so a selected float part shows
# only the game's header and the loader's property rows.  The artwork lives in one
# place - the panel's preview block - and the block is what painted the component's
# picture *and* its pin pictograms ("— R"), so the player's two reports ("底部元件
# 属性栏中可以去掉元件预览图" and "中间这里还有引脚标识没删掉") share the one
# takeover: the preview block is not submitted for a Mod-owned selection.  The
# igImage hook stays armed for the description panel's own picture sites (they are
# the path a component *with* a schematic takes), so the case checks both lines.
Assert-TaskContains 'Component render: panel preview control armed'
Assert-TaskContains 'Component render: panel preview block control armed'
Assert-TaskContains 'Component render: the panel preview block (pin pictogram) is hidden for the selected Mod component'

# The host owns the rows, so the commit is the host's line: the loader logs the
# field it wrote on which instance (one commit = one undo step and the type's
# on_config_changed fires, both of which the component-editor cases cover for
# the service itself).
Assert-TaskContains ('Component editor: committed field "label" on instance 0x')

# And no window of the Mod's own may appear over the board: the old row window is
# gone, which is half the point of moving to the declared rows.
$taskPopup = @($taskLines | Where-Object { $_ -match '###float-ops-rows|float-ops: the drawer is editing' })
if ($taskPopup.Count -ne 0) {
  throw "The Mod still owns a row window although the loader draws the rows (inspect $taskLog)"
}

# What the player typed has to reach the instance's configuration and the board.
# The commit line above is the write; the Mod's own layout trace is the board
# showing the new name (TC_FLOATOPS_LAYOUT=1).
Assert-TaskContains ('name="' + $Keys + '"')

# A write that could not find its instance is the player's "写入元件配置失败", so
# the log must not carry either diagnostic - on the crowded board the instance is
# well past any fixed-size handle array, which is exactly what used to fail.
foreach ($taskDiagnostic in @('no live handle for instance',
                              'the host refused the configuration for instance',
                              'the host would not list its instances')) {
  if ($taskText.Contains($taskDiagnostic)) {
    throw "A configuration write failed ($taskDiagnostic); inspect $taskLog"
  }
}

"PASS float-ops panel input ($taskBoardName board): the rows in the game's own panel took the click, the typing and the commit; the label reached the instance and the board, and no window opened over it"
"Sandbox: $taskSandbox"
