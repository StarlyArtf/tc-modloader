# Real-machine check for the built-in Keyboard rewrite (src/keyboard_fix.hpp).
#
# A board carrying one Keyboard (tools/kind-fixture.py 0x5d) is loaded as the
# sandbox level's board; dev.keyboard-fix-driver pushes a key-down and a key-up
# into the component's ring buffer, steps the simulation and reads the output
# slots back out of the game's own state array.
#
# The sandbox Mod is deliberately *not* loaded here: dev.sandbox-sim suppresses
# the game's own run ("stock run suppressed; simcore owns sandbox state
# updates"), and the subject of this test is exactly the program the game's own
# JIT runs for a Keyboard.
#
#   -Fix       (default) run with the rewrite on: the key code and the key state
#              must appear on the component's outputs.
#   -Disabled  run the same board with TC_MODLOADER_KEYBOARD_FIX=0: the game's
#              own emission is used, and the driver must observe the defect this
#              fix removes - the component reports "an event is waiting" and
#              never the key.
#
# Both directions are asserted because the point of the rewrite is a difference
# in what the game runs, not a line in a dump.
param(
  [switch]$Disabled,
  [int]$Seconds = 70,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskCircuit = Join-Path $taskRepo 'build\kind-fixture-kbd\circuit.data'
$taskTag = if($Disabled) { 'keyboard-fix-disabled' } else { 'keyboard-fix' }
$taskOut = Join-Path $taskRepo ('build\' + $taskTag)
if (!(Test-Path -LiteralPath (Join-Path $taskRepo 'build\kinds.txt'))) {
  & (Join-Path $PSScriptRoot 'kind-list-playtest.ps1')
  if ($LASTEXITCODE) { throw 'The keyboard prototype dump could not be written' }
}
& python (Join-Path $taskRepo 'tools\kind-fixture.py') --out $taskCircuit 0x5d
if ($LASTEXITCODE) { throw 'The keyboard fixture could not be written' }
foreach ($taskRequired in @((Join-Path $taskRepo 'dist\dev.enter-board.mod'),
                            (Join-Path $taskRepo 'dist\dev.keyboard-fix-driver.mod'),
                            (Join-Path $taskRepo 'dist\tc-loader.dll'),
                            (Join-Path $taskRepo 'dist\tcmod-cli.exe'),
                            (Join-Path $taskGame 'Turing Complete.exe'),
                            $taskCircuit)) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}
if (!(Test-Path -LiteralPath (Join-Path $taskRepo 'build\kind-fixture-kbd\make.txt'))) {
  # Record how the board under test was produced, so a rerun is reproducible.
  @('python tools/kind-fixture.py --out build/kind-fixture-kbd/circuit.data 0x5d') |
    Set-Content -LiteralPath (Join-Path $taskRepo 'build\kind-fixture-kbd\make.txt') -Encoding ascii
}

$taskSandbox = Join-Path $taskRepo ('build\' + $taskTag + '-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskSandbox 'game'
$taskProfile = Join-Path $taskSandbox 'home'
$taskProfileDir = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
New-Item -ItemType Directory -Force $taskRoot, $taskProfile, (Join-Path $taskRoot 'mods'),
  (Join-Path $taskProfileDir 'schematics\sandbox\Default') | Out-Null
foreach ($taskDir in @('asset', 'campaign', 'translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll', 'soft_oal.dll',
                        'steam_api64.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
Copy-Item -LiteralPath $taskCircuit -Destination (Join-Path $taskRoot 'campaign\sandbox\circuit.data') -Force
@(
  'setting_window_mode = true'
  'setting_window_position = 0'
  'setting_window_size = 52430000'
  'setting_language = English'
  'setting_current_level = sandbox'
) | Set-Content -LiteralPath (Join-Path $taskProfileDir 'settings.txt') -Encoding ascii
foreach ($taskPackage in @('dev.enter-board.mod', 'dev.keyboard-fix-driver.mod')) {
  Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\' + $taskPackage)) -Destination (Join-Path $taskRoot 'mods') -Force
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.enter-board dev.keyboard-fix-driver | Out-Host
if ($LASTEXITCODE) { throw 'The sandbox Mod apply failed' }

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousDump = $env:TC_MODLOADER_DUMP_SOURCE
$taskPreviousFix = $env:TC_MODLOADER_KEYBOARD_FIX
$taskProcess = $null
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $env:TC_MODLOADER_DUMP_SOURCE = '1'
  if ($Disabled) { $env:TC_MODLOADER_KEYBOARD_FIX = '0' } else { $env:TC_MODLOADER_KEYBOARD_FIX = $null }
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  while ([DateTime]::UtcNow -lt $taskDeadline -and !$taskProcess.HasExited) {
    Start-Sleep -Milliseconds 500
    $taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
    if (Test-Path -LiteralPath $taskLog) {
      $taskDone = Select-String -LiteralPath $taskLog -Pattern 'keyboard-fix: done' -ErrorAction SilentlyContinue
      if ($taskDone) { Start-Sleep -Seconds 2; break }
    }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:TC_MODLOADER_DUMP_SOURCE = $taskPreviousDump
  $env:TC_MODLOADER_KEYBOARD_FIX = $taskPreviousFix
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
}

$taskLogPath = Join-Path $taskRoot 'tc-modloader-data\loader.log'
if (!(Test-Path -LiteralPath $taskLogPath)) { throw "The game wrote no loader log; inspect $taskSandbox" }
New-Item -ItemType Directory -Force $taskOut | Out-Null
Copy-Item -LiteralPath $taskLogPath -Destination (Join-Path $taskOut 'loader.log') -Force
$taskLines = @(Select-String -LiteralPath $taskLogPath -Pattern 'keyboard-fix:' | ForEach-Object { $_.Line })
if (!$taskLines.Count) { throw "The driver never ran; inspect $taskOut\loader.log" }
$taskLines | ForEach-Object { $_ }

function Find-Line([string]$Pattern) {
  return @($taskLines | Where-Object { $_ -match $Pattern }) | Select-Object -Last 1
}
$taskDown = Find-Line 'injected down key=0x41'
$taskAfterDown = Find-Line 'after-down'
$taskAfterUp = Find-Line 'after-up'
foreach ($taskNeedle in @($taskDown, $taskAfterDown, $taskAfterUp)) {
  if (!$taskNeedle) { throw "The driver did not complete its sequence; inspect $taskOut\loader.log" }
}

if ($Disabled) {
  # The game's own emission: the buffer holds the event, the component says one
  # is waiting, and the key code and key state never reach the outputs.
  if ($taskAfterDown -notmatch 'out0=1 out1=0 out2=0x0') {
    throw "Without the rewrite the Keyboard was expected to report a waiting event and no key; got: $taskAfterDown"
  }
  if ($taskAfterUp -notmatch 'out0=1 out1=0 out2=0x0') {
    throw "Without the rewrite the key-up event must also stay invisible; got: $taskAfterUp"
  }
  "PASS keyboard fix disabled control: the game's own Keyboard reports out0=1 with out1=0 out2=0x0 for both events (buffer non-empty, no key and no state on the pins)"
} else {
  # out0 is "an event is still waiting", and with the rewrite it is read as soon
  # as it arrives, so it is back to 0 by the time the frame publishes - while
  # out1 (the state) and out2 (the key code) now carry the event.
  if ($taskAfterDown -notmatch 'out0=0 out1=1 out2=0x41') {
    throw "With the rewrite the key-down must reach the outputs as out1=1 out2=0x41; got: $taskAfterDown"
  }
  if ($taskAfterUp -notmatch 'out0=0 out1=0 out2=0x41') {
    throw "With the rewrite the key-up must clear the state and keep the key code; got: $taskAfterUp"
  }
  $taskDumps = @(Get-ChildItem -LiteralPath $taskRoot -Filter 'native-logic-source-keyboard-*.txt' -ErrorAction SilentlyContinue)
  if (!$taskDumps.Count) { throw "The rewrite ran but wrote no fixed-source dump; inspect $taskRoot" }
  $taskFixed = Get-Content -LiteralPath $taskDumps[0].FullName -Raw
  if ($taskFixed -match 'is_new_value_\d+ && \(U1 0x0\) != 0') {
    throw "The compiled program still gates the read on a constant-zero strobe"
  }
  if ($taskFixed -notmatch 'if is_new_value_\d+ \{') {
    throw "The compiled program does not read an undriven Keyboard unconditionally"
  }
  Copy-Item -LiteralPath $taskDumps[0].FullName -Destination (Join-Path $taskOut $taskDumps[0].Name) -Force
  "PASS keyboard fix: a key pushed into the buffer reached the built-in Keyboard's outputs (out1=1 out2=0x41 on key down, out1=0 out2=0x41 after key up) and the compiled program reads an undriven strobe"
}
"sandbox: $taskSandbox"
