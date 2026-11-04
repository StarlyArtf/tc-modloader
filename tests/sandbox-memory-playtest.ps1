# Real-machine case for the memory subsystem (docs/PLAN-sandbox-simulator.md 15.3).
#
# What it proves: the RAM (0x76) and its Load/Store ports (0x36/0x38) reach a
# board the way the viewer's table now describes them - the RAM declaring no
# pins of its own, the ports carrying the interface - and it records what the
# game actually keeps in those records, which is what a memory device would
# have to be built from.
#
# The board is the Symphony CPU's own: campaign/symphony_6_ram/circuit.data
# holds two RAMs and ten ports and no wires at all, so it is the cleanest
# sample of "memory is reached through ports, not through wires".  The case
# deploys it as the sandbox level (Mods may deploy campaign/, measured in
# docs/research/level-interface.md), runs the game in an isolated copy, and
# reads the raw component records dev.sandbox-sim writes.
param(
  [int]$Seconds = 45,
  [switch]$KeepRunning,
  [string]$CircuitPath = ''
)
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskMod = Join-Path $taskRepo 'dist\dev.sandbox-sim.mod'
$taskCircuit = if ($CircuitPath) {
  (Resolve-Path -LiteralPath $CircuitPath).Path
} else {
  Join-Path $taskGame 'campaign\symphony_6_ram\circuit.data'
}
foreach ($taskRequired in @($taskMod, (Join-Path $taskRepo 'dist\tc-loader.dll'),
                             (Join-Path $taskRepo 'dist\tcmod-cli.exe'),
                             (Join-Path $taskGame 'Turing Complete.exe'),
                             $taskCircuit)) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}

$taskSandbox = Join-Path $taskRepo ('build\sandbox-memory-' + [guid]::NewGuid().ToString('N'))
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
# The memory board becomes the sandbox's starting board.  Nothing else about the
# level is changed: the RAMs and their ports are the whole of it.
Copy-Item -LiteralPath $taskCircuit `
  -Destination (Join-Path $taskRoot 'campaign\sandbox\circuit.data') -Force
# dump=1 is what writes sandbox-sim-components.txt; the viewer is not started and
# no trace is written, so the run stays a read-only measurement.
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
# dev.enter-board enters the level; dev.sandbox-sim binds the board and dumps.
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
  $taskDump = Join-Path $taskData 'sandbox-sim-components.txt'
  while ([DateTime]::UtcNow -lt $taskDeadline -and !$taskProcess.HasExited) {
    Start-Sleep -Milliseconds 500
    if ((Test-Path -LiteralPath $taskDump) -and
        ((Get-Item -LiteralPath $taskDump).Length -gt 4096)) { break }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
}

if (!(Test-Path -LiteralPath $taskDump)) { throw "No component dump; inspect $taskSandbox" }
$taskEvidence = Join-Path $taskRepo 'build\sandbox-memory-components.txt'
Copy-Item -LiteralPath $taskDump -Destination $taskEvidence -Force
$taskRows = @{}
foreach ($taskLine in Get-Content -LiteralPath $taskDump) {
  if ($taskLine -match '^component (\d+) kind=0x(\S+) x=(-?\d+) y=(-?\d+) rotation=(\d+) id=(\d+) word30=(\d+) ptr38=(\d+) word40=(\d+) word88=(\d+) custom=(\d+) worde0=(\d+) worde8=(\d+) wordf0=(\d+) little178=(\d+)') {
    $taskRows[[int]$Matches[1]] = $Matches[0]
  }
}
$taskListed = @($taskRows.Values | Where-Object { $_ -match 'kind=0x(36|38|76) ' })
$taskRams = @($taskRows.Values | Where-Object { $_ -match 'kind=0x76 ' })
$taskLoads = @($taskRows.Values | Where-Object { $_ -match 'kind=0x36 ' })
$taskStores = @($taskRows.Values | Where-Object { $_ -match 'kind=0x38 ' })
# The counts are the board's own (measured 2026-09-29): two RAMs, six Load ports
# - three on the Register File and three on Main Memory, the last of which is
# the named "Instruction" one - and four Store ports.
if ($taskRams.Count -ne 2) { throw "the memory board should carry two RAMs, found $($taskRams.Count)" }
if ($taskLoads.Count -ne 6) { throw "the memory board should carry six Load ports, found $($taskLoads.Count)" }
if ($taskStores.Count -ne 4) { throw "the memory board should carry four Store ports, found $($taskStores.Count)" }
foreach ($taskRow in $taskRams) {
  if ($taskRow -notmatch 'word30=0 ptr38=0 word40=0') { throw "a RAM reports pins: $taskRow" }
  if ($taskRow -notmatch 'worde0=8 worde8=8 wordf0=(0|16) little178=0$') {
    throw "a RAM has unexpected width/capacity/endian fields: $taskRow"
  }
}
foreach ($taskRow in $taskLoads) {
  if ($taskRow -notmatch 'word30=3 .* word40=2') { throw "a Load port is not 3in/2out: $taskRow" }
  if ($taskRow -notmatch 'word88=(8|16|32) .* worde0=\1 worde8=\1 wordf0=0 little178=0$') {
    throw "a Load port's visible/access widths disagree: $taskRow"
  }
}
foreach ($taskRow in $taskStores) {
  if ($taskRow -notmatch 'word30=5 .* word40=1') { throw "a Store port is not 5in/1out: $taskRow" }
  if ($taskRow -notmatch 'word88=0 .* worde0=(8|16|32) worde8=\1 wordf0=0 little178=0$') {
    throw "a Store port has unexpected access-width fields: $taskRow"
  }
}
$taskRamExtra = @($taskRams | ForEach-Object {
  if ($_ -match 'wordf0=(\d+)') { [int]$Matches[1] }
} | Sort-Object)
if (($taskRamExtra -join ',') -ne '0,16') {
  throw "expected Register File/Main Memory extra address bits 0,16; got $($taskRamExtra -join ',')"
}
$taskLogPath = Join-Path $taskRoot 'tc-modloader-data\loader.log'
if (!(Test-Path -LiteralPath $taskLogPath)) { throw "No loader log: $taskLogPath" }
$taskLogText = Get-Content -LiteralPath $taskLogPath -Raw
if ($taskLogText -match 'sandbox-sim: cannot run this board') {
  throw 'simcore refused the real RAM/port board; inspect the sandbox loader.log'
}
if ($taskLogText -notmatch 'sandbox-sim: bound board -> .*devices=10') {
  throw 'simcore did not bind all ten real Load/Store ports; inspect the sandbox loader.log'
}
$taskListed | ForEach-Object { $_ }
"PASS sandbox memory: real records agree on geometry, widths, capacities and endian; simcore bound all ten ports"
"evidence: $taskEvidence"
"sandbox: $taskSandbox"
