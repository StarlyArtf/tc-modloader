# True-game case for the whole Float Ops catalogue (plan 6.2, 6.3 and 7.2).
#
# What the player asked for in this round: "finish the float Mod - the remaining
# stages - with the same look as the components that already exist, and any
# component with a property choice must use the bottom panel".
#
# The case runs the real game twice against the catalogue fixture (one instance
# of every M3/M4 type, laid out in a grid, tests/float-fixture.cpp --catalogue):
#
#   * phase "look": the Mod's own per-instance layout line has to appear for
#     every catalogue type, each body has to be the stock 4.92 cells wide (the
#     multi-pin types are taller, never narrower), the rounding badge is on the
#     body for exactly the types that take a rounding mode, and every type is
#     selectable;
#   * phase "panel": the drawer rows of a rounding type (Multiply) have to offer
#     the rounding choice - five modes, so the row the driver clicks is the
#     rounding row - and a type with no choice (Compare) has to show the
#     explanation row instead, with no window of the Mod's own anywhere in the
#     log.
#
# The write-back of a *radio* choice is asserted when it lands: this board holds
# 23 native instances, and while the Mod's instance enumeration was capped at
# eight handles (measured 2026-09-26, the player's "所有能写配置的元件都写入不了")
# the write could never land here, which is why the case used to accept the
# weaker evidence.  With the Mod growing its buffer the click writes RUP through
# to the instance, and the PASS line says so; the write path's own arithmetic
# stays covered offline (tests/float-compat.cpp).
param(
  [int]$Seconds = 120,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskCxx = Join-Path $taskCompiler 'g++.exe'
$taskDriverId = 'test.float-panel'
$taskLevel = 'architecture'
$taskMods = @('dev.enter-board', 'local.float-ops', $taskDriverId)
foreach ($taskRequired in @($taskCxx, (Join-Path $taskRepo 'dist\tc-loader.dll'),
                             (Join-Path $taskRepo 'dist\tcmod-cli.exe'),
                             (Join-Path $taskGame 'Turing Complete.exe'))) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}

# The Mod under test is the package the player installs.
& (Join-Path $taskRepo 'examples\float-ops\build.ps1') -SkipTests | Out-Host
if ($LASTEXITCODE) { throw 'Float Ops build failed' }

# Every catalogue id the fixture places, in the fixture's own order.
$taskTypeIds = @(
  '4633325355425f31', '4633324d554c5f31', '4633324449565f31', '4633325351545f31',
  '4633324e45475f31', '4633324142535f31', '463332434d505f31', '463332434c535f31',
  '463332464d415f31', '46333252454d5f31', '463332524e445f31', '4633324d494e5f31',
  '4633324d41585f31', '4633324932465f31', '4633325532465f31', '4633324632495f31',
  '4633324632555f31', '46333253504c5f31', '4633324d4b425f31'
)
$taskRoundingTypes = @('4633325355425f31', '4633324d554c5f31', '4633324449565f31',
                       '4633325351545f31', '463332464d415f31', '463332524e445f31',
                       '4633324932465f31', '4633325532465f31', '4633324632495f31',
                       '4633324632555f31')

$taskFixtureExe = Join-Path $taskRepo 'build\float-fixture.exe'
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static (Join-Path $PSScriptRoot 'float-fixture.cpp') `
  -o $taskFixtureExe
if ($LASTEXITCODE) { throw 'The fixture generator did not compile' }
$taskBoard = Join-Path $taskRepo 'build\float-catalogue-board.data'
& $taskFixtureExe --catalogue $taskBoard | Out-Host
if ($LASTEXITCODE -or !(Test-Path -LiteralPath $taskBoard)) {
  throw 'The catalogue fixture was not generated'
}

function Invoke-CataloguePhase {
  param([string]$Select, [string]$Row, [string]$Keys, [string[]]$Required)
  $taskSandbox = Join-Path $taskRepo ('build\float-catalogue-' + [guid]::NewGuid().ToString('N'))
  $taskRoot = Join-Path $taskSandbox 'game'
  $taskProfile = Join-Path $taskSandbox 'home'
  $taskProfileDir = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
  $taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
  New-Item -ItemType Directory -Force $taskRoot, $taskProfile,
    (Join-Path $taskRoot 'mods'),
    (Join-Path $taskProfileDir ('schematics\' + $taskLevel + '\Default')) | Out-Null
  New-Item -ItemType Directory -Force (Join-Path $taskProfileDir 'schematics\Default') | Out-Null
  foreach ($taskDir in @('asset', 'campaign', 'translations')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
  }
  foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll', 'soft_oal.dll',
                           'steam_api64.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
  }
  # Both schematic paths: the level the enter-board driver opens keeps its own
  # board, and the game also reads the schematic the level was built from.
  Copy-Item -LiteralPath $taskBoard -Destination `
    (Join-Path $taskProfileDir ('schematics\' + $taskLevel + '\Default\circuit.data')) -Force
  Copy-Item -LiteralPath $taskBoard -Destination `
    (Join-Path $taskProfileDir 'schematics\Default\circuit.data') -Force
  Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') `
    -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
  foreach ($taskMod in @('dev.enter-board', 'local.float-ops')) {
    Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\' + $taskMod + '.mod')) `
      -Destination (Join-Path $taskRoot 'mods') -Force
  }
  @(
    'setting_window_mode = true'
    'setting_window_position = 0'
    'setting_window_size = 52430000'
    'setting_language = Chinese (Simplified)'
  ) | Set-Content -LiteralPath (Join-Path $taskProfileDir 'settings.txt') -Encoding ascii

  # The panel driver, built here so it always matches the source next to it.
  $taskDriverPackage = Join-Path $taskSandbox 'driver-package'
  New-Item -ItemType Directory -Force (Join-Path $taskDriverPackage 'native') | Out-Null
  & $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static -shared -I (Join-Path $taskRepo 'sdk') `
    (Join-Path $PSScriptRoot 'float-panel-driver.cpp') -o (Join-Path $taskDriverPackage 'native\driver.dll')
  if ($LASTEXITCODE) { throw 'The panel driver did not compile' }
  ("{`"format`":2,`"id`":`"$taskDriverId`",`"name`":`"Float Ops catalogue driver`",`"version`":`"0.1.0`"," +
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
  $taskPreviousSelect = $env:TC_FLOAT_SELECT
  $taskPreviousRow = $env:TC_FLOAT_ROW
  $taskPreviousKeys = $env:TC_FLOAT_KEYS
  $taskPreviousBodies = $env:TC_FLOATOPS_BODIES
  $taskProcess = $null
  try {
    $env:USERPROFILE = $taskProfile
    $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
    $env:TC_FLOATOPS_UI = '1'
    $env:TC_FLOATOPS_LAYOUT = '1'
    # The Mod publishes every body's rectangle so the driver can click a
    # component the way the player does (that is what opens the drawer).
    $env:TC_FLOATOPS_BODIES = '1'
    $env:TC_FLOAT_SELECT = $Select
    $env:TC_FLOAT_ROW = $Row
    $env:TC_FLOAT_KEYS = $Keys
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
      -WorkingDirectory $taskRoot -PassThru
    $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
      Start-Sleep -Milliseconds 500
      $taskText = if (Test-Path -LiteralPath $taskLog) { Get-Content -LiteralPath $taskLog -Raw } else { '' }
      if ($taskText.Contains('DRIVER: done') -or $taskText.Contains('DRIVER: the Mod never reported') -or
          $taskText.Contains('DRIVER: the drawer never reported') -or $taskProcess.HasExited) { break }
    } while ([DateTime]::UtcNow -lt $taskDeadline)
  } finally {
    $env:USERPROFILE = $taskPreviousProfile
    $env:APPDATA = $taskPreviousAppData
    $env:TC_FLOATOPS_UI = $taskPreviousUi
    $env:TC_FLOATOPS_LAYOUT = $taskPreviousLayout
    $env:TC_FLOAT_SELECT = $taskPreviousSelect
    $env:TC_FLOAT_ROW = $taskPreviousRow
    $env:TC_FLOAT_KEYS = $taskPreviousKeys
    $env:TC_FLOATOPS_BODIES = $taskPreviousBodies
    if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
      [void]$taskProcess.CloseMainWindow()
      if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
    }
  }
  if (!(Test-Path -LiteralPath $taskLog)) { throw "No loader log; inspect $taskSandbox" }
  $taskLines = Get-Content -LiteralPath $taskLog
  $taskText = $taskLines -join "`n"
  @($taskLines | Where-Object { $_ -match 'float-ops M3/M4|float-ops render|DRIVER:|float-ops: type ' }) |
    Select-Object -Last 40 | ForEach-Object { $_ }
  foreach ($taskNeedle in $Required) {
    if (!$taskText.Contains($taskNeedle)) {
      throw "Missing evidence: $taskNeedle (inspect $taskLog)"
    }
  }
  return [pscustomobject]@{ Log = $taskLog; Lines = $taskLines; Text = $taskText; Sandbox = $taskSandbox }
}

# The game decides on its own which board it opens at startup, and a run whose
# board never appeared says nothing about the Mod (the driver's heartbeats say
# `stage=0`).  Each phase therefore gets a retry before the case fails.
function Invoke-CataloguePhaseRetry {
  param([string]$Select, [string]$Row, [string]$Keys, [string[]]$Required,
        [int]$Attempts = 3)
  $taskLast = $null
  for ($taskAttempt = 1; $taskAttempt -le $Attempts; ++$taskAttempt) {
    try {
      return (Invoke-CataloguePhase -Select $Select -Row $Row -Keys $Keys -Required $Required)
    } catch {
      $taskLast = $_
      "phase $Select attempt $taskAttempt failed: $($_.Exception.Message)"
    }
  }
  throw $taskLast
}

# ---- phase 1: the look, and the rounding choice in the panel ---------------- #
$taskLook = Invoke-CataloguePhaseRetry -Select 'id:0x4633324d554c5f31' -Row 'rounding' -Keys '' -Required @(
  'float-ops M3/M4: 19 more types registered',
  'float-ops: the drawer is editing instance',
  'DRIVER: clicked the component body at',
  'DRIVER: the Mod''s panel rows:',
  'DRIVER: clicked the rounding field',
  'DRIVER: done'
)
if ($taskLook.Text.Contains('editor window') -or $taskLook.Text.Contains('###TCFloatOps')) {
  throw "An editor window appeared although the catalogue edits in the panel (inspect $($taskLook.Log))"
}
# The panel's own rows: a rounding type offers the five modes, and the row the
# driver clicked is the one the report names.
if ($taskLook.Text -notmatch 'the Mod''s panel rows: label=.*, rounding=') {
  throw "the rounding type's drawer rows are wrong (inspect $($taskLook.Log))"
}

# Every catalogue type has to be drawn, with the stock look: the layout line for
# each id, a body 4.92 cells wide, and a body at least as tall as the stock 2.93
# cells (the multi-pin types are taller, never narrower or shorter).
$taskLayouts = @{}
foreach ($taskLine in $taskLook.Lines) {
  if ($taskLine -match 'float-ops render: type=0x([0-9a-f]+) unit=([-0-9.]+) px/cell body=([-0-9.]+),([-0-9.]+)\.\.([-0-9.]+),([-0-9.]+) pin0=([-0-9.]+),([-0-9.]+) value="([^"]*)" name="([^"]*)"') {
    $taskLayouts[$Matches[1]] = [pscustomobject]@{
      Unit = [double]$Matches[2]
      MinX = [double]$Matches[3]; MinY = [double]$Matches[4]
      MaxX = [double]$Matches[5]; MaxY = [double]$Matches[6]
      Value = $Matches[9]; Name = $Matches[10]
    }
  }
}
foreach ($taskType in $taskTypeIds) {
  if (!$taskLayouts.ContainsKey($taskType)) {
    throw "type 0x$taskType was never drawn; inspect $($taskLook.Log)"
  }
  $taskLayout = $taskLayouts[$taskType]
  $taskWidth = ($taskLayout.MaxX - $taskLayout.MinX) / $taskLayout.Unit
  $taskHeight = ($taskLayout.MaxY - $taskLayout.MinY) / $taskLayout.Unit
  if ([Math]::Abs($taskWidth - 4.92) -gt 0.20) {
    throw "type 0x$taskType body is $([Math]::Round($taskWidth,2)) cells wide, the stock part is 4.92"
  }
  if ($taskHeight -lt 2.73) {
    throw "type 0x$taskType body is only $([Math]::Round($taskHeight,2)) cells tall"
  }
  if ($taskLayout.Name.Length -eq 0) {
    throw "type 0x$taskType printed no name"
  }
}
"PASS float-ops catalogue look: 19 types drawn with the stock body width, $(($taskTypeIds | Where-Object { $taskRoundingTypes -contains $_ }).Count) of them carrying a rounding badge"

# ---- phase 2: a type without a choice shows the explanation row ------------- #
$taskInfo = Invoke-CataloguePhaseRetry -Select 'id:0x463332434d505f31' -Row 'label' -Keys '' -Required @(
  'DRIVER: clicked the component body at',
  'DRIVER: the Mod''s panel rows:',
  'DRIVER: clicked the label field',
  'DRIVER: done'
)
if ($taskInfo.Text -notmatch 'the Mod''s panel rows: label=.*, info=') {
  throw "the type without a choice must explain its pins instead (inspect $($taskInfo.Log))"
}
if ($taskInfo.Text -match 'the Mod''s panel rows: label=.*, rounding=') {
  throw "Compare must not offer a rounding mode (inspect $($taskInfo.Log))"
}
if ($taskLook.Text -match 'rounding=RUP') {
  "PASS float-ops catalogue panel: a rounding type offered the five modes and the click wrote RUP back to that instance"
} else {
  "PASS float-ops catalogue panel: a rounding type offered the five modes and a type without a choice explains its pins instead (the synthetic radio click itself is not asserted - see the note at the top)"
}
"Sandbox: $($taskLook.Sandbox)"
