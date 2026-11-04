# Real-machine case for the storage family (Register 0x27, Counter 0x26, Delay
# Line 0x37) in the sandbox simulator.
#
# What it proves: the pins the prototype table keeps out of its declared input
# list - a register's enable and data, a counter's reset, a delay line's input -
# are where the builder now looks for them, the three parts get devices, and a
# board carrying them binds and advances instead of being refused with "has no
# device in the library".
#
# The board is written by tools/kind-fixture.py: a level input into each kind's
# data/control pins and an Output component on each output pin, so the game
# itself compiles it (which is how the behaviour was read), and then the same
# board is handed to the sandbox simulator.
param(
  [int]$Seconds = 45,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskMod = Join-Path $taskRepo 'dist\dev.sandbox-sim.mod'
$taskFixtureDir = Join-Path $taskRepo 'build\sandbox-storage-fixture'
$taskCircuit = Join-Path $taskFixtureDir 'circuit.data'
foreach ($taskRequired in @($taskMod, (Join-Path $taskRepo 'dist\tc-loader.dll'),
                            (Join-Path $taskRepo 'dist\tcmod-cli.exe'),
                            (Join-Path $taskGame 'Turing Complete.exe'))) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}
# The fixture is generated, not checked in: it is a handful of records the game's
# own prototype table gave the pin offsets for.
& python (Join-Path $taskRepo 'tools\kind-fixture.py') --out $taskCircuit --standalone 0x27 0x26 0x37
if ($LASTEXITCODE) { throw 'The storage fixture could not be written' }

$taskSandbox = Join-Path $taskRepo ('build\sandbox-storage-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskSandbox 'game'
$taskProfile = Join-Path $taskSandbox 'home'
$taskProfileDir = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.sandbox-sim'
New-Item -ItemType Directory -Force $taskRoot, $taskProfile, (Join-Path $taskRoot 'mods'), $taskData,
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
  'enabled=1'
  'dump=1'
  'view=0'
  'vcd=0'
  'startup_seed=1'
  'own_cycle=1'
  'trace=1'
) | Set-Content -LiteralPath (Join-Path $taskData 'sandbox-sim.txt') -Encoding ascii
@(
  'setting_window_mode = true'
  'setting_window_position = 0'
  'setting_window_size = 52430000'
  'setting_language = English'
  'setting_current_level = sandbox'
) | Set-Content -LiteralPath (Join-Path $taskProfileDir 'settings.txt') -Encoding ascii
foreach ($taskPackage in @('dev.sandbox-sim.mod', 'dev.enter-board.mod')) {
  Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\' + $taskPackage)) -Destination (Join-Path $taskRoot 'mods') -Force
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.sandbox-sim dev.enter-board | Out-Host
if ($LASTEXITCODE) { throw 'The sandbox Mod apply failed' }

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskProcess = $null
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  $taskReport = Join-Path $taskData 'sandbox-sim-report.txt'
  $taskLogPath = Join-Path $taskRoot 'tc-modloader-data\loader.log'
  # The report is written from the first frame; what the case is waiting for is
  # the *binding*, which happens once the level is up.  Wait for either verdict
  # line or the deadline, so a slow sandbox is not mistaken for a refusal.
  while ([DateTime]::UtcNow -lt $taskDeadline -and !$taskProcess.HasExited) {
    Start-Sleep -Milliseconds 500
    if (Test-Path -LiteralPath $taskLogPath) {
      $taskSeen = Get-Content -LiteralPath $taskLogPath -Raw
      if ($taskSeen -match 'sandbox-sim: bound board' -or $taskSeen -match 'sandbox-sim: cannot run this board') { break }
    }
  }
  # The report is throttled and written from the frame that binds onwards, so
  # give the runtime a few cycles to advance and rewrite it before stopping.
  if (!$taskProcess.HasExited) { Start-Sleep -Seconds 6 }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
}

if (!(Test-Path -LiteralPath $taskLogPath)) { throw "No loader log: $taskLogPath" }
$taskLogText = Get-Content -LiteralPath $taskLogPath -Raw
if ($taskLogText -match 'sandbox-sim: cannot run this board') {
  $taskReason = [regex]::Match($taskLogText, 'sandbox-sim: cannot run this board: [^\r\n]*').Value
  throw "simcore refused the storage board: $taskReason"
}
if ($taskLogText -notmatch 'sandbox-sim: bound board -> .*devices=4') {
  throw 'simcore did not bind the constant, the register, the counter and the delay line as four devices'
}
$taskDevices = [regex]::Match($taskLogText, 'sandbox-sim: devices -> [^\r\n]*').Value
foreach ($taskWanted in @('_0_register', '_2_counter', '_1_delay')) {
  if ($taskDevices -notmatch $taskWanted) {
    throw "simcore did not turn the$taskWanted kind into a device: $taskDevices"
  }
}
$taskEvidence = Join-Path $taskRepo 'build\sandbox-storage-report.txt'
if (Test-Path -LiteralPath $taskReport) { Copy-Item -LiteralPath $taskReport -Destination $taskEvidence -Force }
"PASS sandbox storage: the constant, register, counter and delay line become four devices"
"$taskDevices"
"evidence: $taskEvidence"
"sandbox: $taskSandbox"
