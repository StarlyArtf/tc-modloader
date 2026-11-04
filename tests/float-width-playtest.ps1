# True-game case for the width cut (docs/PLAN-float-components.md, section
# 6.4.1): the same Constant -> Add -> Display chain built three times, once in
# binary16, once in binary32 and once in binary64, in the shipped package and
# the real game.
#
# Phase 1 proves the chains themselves: the three *types* have their own pin
# widths (16, 32 and 64 bits - the loader's own registration lines say so), the
# game compiles them into one board, each callback runs in its own format, and
# each display prints its result in that format's text.  A Mod that computed
# everything in binary32 would still compile this board, so the assertions are on
# the values: 1.0 + 1.0 has to read "2 0x4000" in binary16 and
# "2 0x4000000000000000" in binary64, not the eight-hex-digit binary32 form.
#
# Phases 2 and 3 prove the *editing* of the new widths: the panel driver (the
# same one the FP32 drawer case uses) selects an FP16 constant, clicks its 值 row
# and types "3.5" with real mouse and keyboard input; the loader's own decimal
# field parses that into the binary16 pattern, the commit line names the value,
# and the board's display shows 3.5 + 1.0 = 4.5 in binary16.  The FP64 constant
# gets the same treatment with "2.5".  Before this round the loader's float row
# was four bytes of binary32, so those two types had only the hexadecimal row.
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
$taskMods = @('dev.enter-board', 'local.float-ops')
foreach ($taskRequired in @($taskCxx, (Join-Path $taskRepo 'dist\tc-loader.dll'),
                             (Join-Path $taskRepo 'dist\tcmod-cli.exe'),
                             (Join-Path $taskGame 'Turing Complete.exe'))) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}

# The Mod under test is the package the player installs.
& (Join-Path $taskRepo 'examples\float-ops\build.ps1') -SkipTests | Out-Host
if ($LASTEXITCODE) { throw 'Float Ops build failed' }

# The board: one Constant/Add/Display chain per format, wired into three chains.
$taskFixtureExe = Join-Path $taskRepo 'build\float-fixture.exe'
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static `
  (Join-Path $PSScriptRoot 'float-fixture.cpp') -o $taskFixtureExe
if ($LASTEXITCODE) { throw 'The fixture generator did not compile' }
$taskBoard = Join-Path $taskRepo 'build\float-widths-board.data'
& $taskFixtureExe --widths $taskBoard | Out-Host
if ($LASTEXITCODE -or !(Test-Path -LiteralPath $taskBoard)) {
  throw 'The widths fixture was not generated'
}

function Invoke-WidthPhase {
  param(
    [string]$Select = '',
    [string]$Row = '',
    [string]$Keys = '',
    [string[]]$Required = @()
  )
  $taskSandbox = Join-Path $taskRepo ('build\float-widths-' + [guid]::NewGuid().ToString('N'))
  $taskRoot = Join-Path $taskSandbox 'game'
  $taskProfile = Join-Path $taskSandbox 'home'
  $taskProfileDir = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
  $taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
  New-Item -ItemType Directory -Force $taskRoot, $taskProfile,
    (Join-Path $taskRoot 'mods'),
    (Join-Path $taskProfileDir ('schematics\' + $taskLevel + '\Default')),
    (Join-Path $taskProfileDir 'schematics\Default') | Out-Null
  foreach ($taskDir in @('asset', 'campaign', 'translations')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
  }
  foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll', 'soft_oal.dll',
                           'steam_api64.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
  }
  Copy-Item -LiteralPath $taskBoard -Destination `
    (Join-Path $taskProfileDir ('schematics\' + $taskLevel + '\Default\circuit.data')) -Force
  Copy-Item -LiteralPath $taskBoard -Destination `
    (Join-Path $taskProfileDir 'schematics\Default\circuit.data') -Force
  Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') `
    -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
  foreach ($taskMod in $taskMods) {
    Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\' + $taskMod + '.mod')) `
      -Destination (Join-Path $taskRoot 'mods') -Force
  }
  @(
    'setting_window_mode = true'
    'setting_window_position = 0'
    'setting_window_size = 52430000'
    'setting_language = Chinese (Simplified)'
    ('setting_current_level = ' + $taskLevel)
  ) | Set-Content -LiteralPath (Join-Path $taskProfileDir 'settings.txt') -Encoding ascii

  # The panel driver is the real-input path into the game's own drawer; it is
  # built here so it always matches the source next to it.
  if ($Select) {
    $taskDriverPackage = Join-Path $taskSandbox 'driver-package'
    New-Item -ItemType Directory -Force (Join-Path $taskDriverPackage 'native') | Out-Null
    & $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static -shared `
      -I (Join-Path $taskRepo 'sdk') (Join-Path $PSScriptRoot 'float-panel-driver.cpp') `
      -o (Join-Path $taskDriverPackage 'native\driver.dll')
    if ($LASTEXITCODE) { throw 'The panel driver did not compile' }
    ("{`"format`":2,`"id`":`"$taskDriverId`",`"name`":`"Float width driver`",`"version`":`"0.1.0`"," +
     "`"native`":{`"api`":1,`"entry`":`"native/driver.dll`"}}") |
      Set-Content -LiteralPath (Join-Path $taskDriverPackage 'mod.json') -Encoding ascii
    & (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskDriverPackage `
      -Output (Join-Path $taskRoot ('mods\' + $taskDriverId + '.mod')) | Out-Null
    if ($LASTEXITCODE) { throw 'The panel driver package was not written' }
  }
  $taskApplied = @($taskMods + $(if ($Select) { $taskDriverId }))
  & (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskApplied | Out-Host
  if ($LASTEXITCODE) { throw 'The sandbox Mod apply failed' }

  $taskPreviousProfile = $env:USERPROFILE
  $taskPreviousAppData = $env:APPDATA
  $taskPreviousLayout = $env:TC_FLOATOPS_LAYOUT
  $taskPreviousUi = $env:TC_FLOATOPS_UI
  $taskPreviousBodies = $env:TC_FLOATOPS_BODIES
  $taskPreviousTrace = $env:TC_EDITOR_TRACE
  $taskPreviousRows = $env:TC_FLOAT_ROWS
  $taskPreviousSelect = $env:TC_FLOAT_SELECT
  $taskPreviousRow = $env:TC_FLOAT_ROW
  $taskPreviousKeys = $env:TC_FLOAT_KEYS
  $taskProcess = $null
  try {
    $env:USERPROFILE = $taskProfile
    $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
    # Every frame's layout line, so the three new types' bodies are in the log
    # even though the Mod only prints the first three types otherwise.
    $env:TC_FLOATOPS_LAYOUT = '1'
    if ($Select) {
      $env:TC_FLOATOPS_UI = '1'
      $env:TC_FLOATOPS_BODIES = '1'
      $env:TC_EDITOR_TRACE = '1'
      $env:TC_FLOAT_ROWS = $taskLog
      $env:TC_FLOAT_SELECT = $Select
      $env:TC_FLOAT_ROW = $Row
      $env:TC_FLOAT_KEYS = $Keys
    } else {
      $env:TC_FLOATOPS_UI = $null
      $env:TC_FLOAT_SELECT = $null
    }
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
      -WorkingDirectory $taskRoot -PassThru
    $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    $taskMarker = if ($Select) { 'DRIVER: done' } else { '0x4000000000000000' }
    do {
      Start-Sleep -Milliseconds 500
      $taskText = if (Test-Path -LiteralPath $taskLog) {
        Get-Content -LiteralPath $taskLog -Raw -Encoding UTF8
      } else { '' }
      if ($taskProcess.HasExited) { break }
    } while ([DateTime]::UtcNow -lt $taskDeadline -and !$taskText.Contains($taskMarker))
  } finally {
    $env:USERPROFILE = $taskPreviousProfile
    $env:APPDATA = $taskPreviousAppData
    $env:TC_FLOATOPS_LAYOUT = $taskPreviousLayout
    $env:TC_FLOATOPS_UI = $taskPreviousUi
    $env:TC_FLOATOPS_BODIES = $taskPreviousBodies
    $env:TC_EDITOR_TRACE = $taskPreviousTrace
    $env:TC_FLOAT_ROWS = $taskPreviousRows
    $env:TC_FLOAT_SELECT = $taskPreviousSelect
    $env:TC_FLOAT_ROW = $taskPreviousRow
    $env:TC_FLOAT_KEYS = $taskPreviousKeys
    if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
      [void]$taskProcess.CloseMainWindow()
      if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
    }
  }
  if (!(Test-Path -LiteralPath $taskLog)) { throw "No loader log; inspect $taskSandbox" }
  $taskLines = Get-Content -LiteralPath $taskLog -Encoding UTF8
  $taskText = $taskLines -join "`n"
  @($taskLines | Where-Object { $_ -match 'float-ops catalogue|Native logic: registered custom 0x4631|Native logic: registered custom 0x4636|float-ops M2: display|Component editor|DRIVER:' }) |
    Select-Object -Last 26 | ForEach-Object { $_ }
  foreach ($taskNeedle in $Required) {
    if (!$taskText.Contains($taskNeedle)) {
      throw "Missing evidence: $taskNeedle (inspect $taskLog)"
    }
  }
  return [pscustomobject]@{ Log = $taskLog; Lines = $taskLines; Text = $taskText; Sandbox = $taskSandbox }
}

# The drawer phases depend on a synthetic click landing on a row whose rectangle
# the loader traces every second, and on the game processing the typed keys
# before the driver moves on.  A run where the row never took the click says
# nothing about the Mod (measured: the same case passed three times and missed
# twice under load, with `active=1` never appearing for the row), so each phase
# gets a retry before the case fails - the same rule the catalogue case uses.
function Invoke-WidthPhaseRetry {
  param([string]$Select, [string]$Row, [string]$Keys, [scriptblock]$Validate,
        [int]$Attempts = 3)
  $taskLast = $null
  for ($taskAttempt = 1; $taskAttempt -le $Attempts; ++$taskAttempt) {
    try {
      $taskPhase = Invoke-WidthPhase -Select $Select -Row $Row -Keys $Keys -Required @('DRIVER: done')
      # The evidence itself is what a retry is for: a run where the row never took
      # the click has to fail here so the next attempt starts a fresh game.
      & $Validate $taskPhase
      return $taskPhase
    } catch {
      $taskLast = $_
      "phase $Select attempt $taskAttempt failed: $($_.Exception.Message)"
    }
  }
  throw $taskLast
}

# ---- phase 1: the three chains, one per format ------------------------------ #
$taskWidths = Invoke-WidthPhase -Required @(
  'float-ops catalogue: 65 types registered (FP16 22, FP32 22, FP64 21'
)
# The loader's own registration lines are the ones that say the pins are 16 and
# 64 bits wide (this is the loader talking, not the Mod).
if (!($taskWidths.Text.Contains('Native logic: registered custom 0x4631364144445f31 inputs=2 outputs=2 in0=(-3,0,w16) in1=(-3,1,w16) out0=(3,0,w16) out1=(3,1,w5)'))) {
  throw "the binary16 add did not register 16-bit pins (inspect $($taskWidths.Log))"
}
if (!($taskWidths.Text.Contains('Native logic: registered custom 0x4636344144445f31 inputs=2 outputs=2 in0=(-3,0,w64) in1=(-3,1,w64) out0=(3,0,w64) out1=(3,1,w5)'))) {
  throw "the binary64 add did not register 64-bit pins (inspect $($taskWidths.Log))"
}
# Each chain computed in its own format and printed in its own format's text:
# 1.0 + 1.0 in binary16, binary32 and binary64.  The hex digit count is the part
# that would be wrong if one binary32 formatter served all three.
if (!($taskWidths.Text -match 'float-ops M2: display 0x[0-9a-f]+ shows 2 0x4000 \(bits=0x4000\)')) {
  throw "the binary16 chain did not report 1.0 + 1.0 in binary16 (inspect $($taskWidths.Log))"
}
if (!($taskWidths.Text -match 'float-ops M2: display 0x[0-9a-f]+ shows 2 0x40000000 \(bits=0x40000000\)')) {
  throw "the binary32 chain did not report 1.0 + 1.0 in binary32 (inspect $($taskWidths.Log))"
}
if (!($taskWidths.Text -match 'float-ops M2: display 0x[0-9a-f]+ shows 2 0x4000000000000000 \(bits=0x4000000000000000\)')) {
  throw "the binary64 chain did not report 1.0 + 1.0 in binary64 (inspect $($taskWidths.Log))"
}
# And the new types are drawn on the board, with their own per-width value text.
foreach ($taskType in @('0x463136434f4e5331', '0x4631364144445f31', '0x4631364449535031',
                        '0x463634434f4e5331', '0x4636344144445f31', '0x4636344449535031')) {
  if (!($taskWidths.Text -match ('float-ops render: type=' + $taskType + ' unit='))) {
    throw "type $taskType was never drawn (inspect $($taskWidths.Log))"
  }
}
if (!($taskWidths.Text -match 'float-ops render: type=0x463634434f4e5331 unit=[-0-9.]+ px/cell body=[-0-9.,]+ pin0=[-0-9.,]+ value="1" name="CONST"')) {
  throw "the binary64 constant's body did not print its own value (inspect $($taskWidths.Log))"
}
"PASS float-ops widths (chains): the shipped package registered 65 types (FP16 22, FP32 22, FP64 21), the game compiled the same Constant -> Add -> Display chain in binary16 and binary64 beside the binary32 one (the loader's own lines show w16 and w64 pins), and each display printed 1.0 + 1.0 in its own format's text - 2 0x4000, 2 0x40000000 and 2 0x4000000000000000"

# ---- phase 2: typing a decimal into a binary16 constant --------------------- #
$taskHalf = Invoke-WidthPhaseRetry -Select 'id:0x463136434f4e5331' -Row 'value' -Keys '3.5' `
  -Validate {
    param($taskPhase)
    if (!$taskPhase.Text.Contains('Component editor: type 0x463136434f4e5331 edits value(float16), bits(uint), label(text)')) {
      throw "the FP16 constant's drawer rows are not the width-aware ones (inspect $($taskPhase.Log))"
    }
    # Which of the two constants the driver's click lands on depends on the camera
    # and on which body is picked first (both are the same type and both feed the
    # adder), so the commit is accepted on either of them: instance 0x…00 is
    # input A and 0x…01 is input B, and both make the chain show 3.5 + 1.0.
    if (!($taskPhase.Text -match 'Component editor: committed field "value" on instance 0x400000000000000[01] value=3\.5')) {
      throw "typing 3.5 into a binary16 constant did not commit (inspect $($taskPhase.Log))"
    }
    if (!($taskPhase.Text -match 'float-ops M2: display 0x[0-9a-f]+ shows 4\.5 0x4480 \(bits=0x4480\)')) {
      throw "3.5 typed into the binary16 constant did not reach the board (inspect $($taskPhase.Log))"
    }
  }
"PASS float-ops widths (binary16 edit): the drawer's 值 row is the loader's width-aware decimal field (value(float16)), real keyboard input of ""3.5"" committed as the binary16 pattern 0x4300, and the chain then showed 3.5 + 1.0 = 4.5 0x4480 on the board"

# ---- phase 3: the same for binary64 ---------------------------------------- #
$taskDouble = Invoke-WidthPhaseRetry -Select 'id:0x463634434f4e5331' -Row 'value' -Keys '2.5' `
  -Validate {
    param($taskPhase)
    if (!$taskPhase.Text.Contains('Component editor: type 0x463634434f4e5331 edits value(float64), bits(uint), label(text)')) {
      throw "the FP64 constant's drawer rows are not the width-aware ones (inspect $($taskPhase.Log))"
    }
    if (!($taskPhase.Text -match 'Component editor: committed field "value" on instance 0x400000000000000[89] value=2\.5')) {
      throw "typing 2.5 into a binary64 constant did not commit (inspect $($taskPhase.Log))"
    }
    if (!($taskPhase.Text -match 'float-ops M2: display 0x[0-9a-f]+ shows 3\.5 0x400C000000000000 \(bits=0x400C000000000000\)')) {
      throw "2.5 typed into the binary64 constant did not reach the board (inspect $($taskPhase.Log))"
    }
  }
"PASS float-ops widths (binary64 edit): the drawer's 值 row is the loader's width-aware decimal field (value(float64)), real keyboard input of ""2.5"" committed as the binary64 pattern 0x4004000000000000, and the chain then showed 2.5 + 1.0 = 3.5 0x400C000000000000 on the board"

"sandboxes: $($taskWidths.Sandbox) | $($taskHalf.Sandbox) | $($taskDouble.Sandbox)"
