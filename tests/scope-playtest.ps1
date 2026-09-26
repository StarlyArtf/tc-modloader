# Real-machine case for the scope panel (local.scope): the board panel that
# samples the board's wires once per cycle through tc.sim.capture.
#
# What this case is for: the panel was built on two services, and both have to
# work through the game, not just in a fake host -
#
#   * tc.sim.channel: the lanes come from the board's own wire records (state
#     byte offset + width), read through the board object snapshot.  A panel that
#     cannot see a wire has no lane to draw;
#   * tc.sim.capture: the loader instruments the compiled program with a
#     per-cycle tick, and the ring it fills has to hold a window with gaps = 0
#     while a real level runs.
#
# The level comes from example.byte-adder's autotest (it loads byte_adder, the
# player's built-in adder, compiles and runs it), so this script only has to run
# the game and read the log.
param(
  [string]$Sandbox = (Join-Path (Split-Path $PSScriptRoot) 'build\scope-panel-sandbox'),
  [int]$Seconds = 75,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskRoot = Join-Path $Sandbox 'game'
$taskHome = Join-Path $Sandbox 'home'
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskLevel = 'byte_adder'

& (Join-Path $PSScriptRoot 'make-ui-sandbox.ps1') -Sandbox $Sandbox `
  -Mods example.byte-adder,local.scope
"Loader under test: $((Get-FileHash (Join-Path $taskRoot 'game_engine.dll') -Algorithm SHA256).Hash)"

# The level's own test needs a circuit: the built-in 8-bit adder.
$taskSchema = Join-Path $taskHome "AppData\Roaming\Turing Complete Mods\profiles\default\schematics\$taskLevel\Default"
New-Item -ItemType Directory -Force $taskSchema | Out-Null
$taskSolution = Join-Path $taskRepo 'build\nl_adder8board.data'
if (!(Test-Path -LiteralPath $taskSolution)) { throw "Missing $taskSolution; run build.ps1 first" }
Copy-Item -LiteralPath $taskSolution -Destination (Join-Path $taskSchema 'circuit.data') -Force
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.byte-adder'
New-Item -ItemType Directory -Force $taskData | Out-Null
Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value $taskLevel -Encoding ascii
Remove-Item -LiteralPath $taskLog -ErrorAction SilentlyContinue

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskProcess = $null
$taskText = ''
try {
  $env:USERPROFILE = $taskHome
  $env:APPDATA = Join-Path $taskHome 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = (Get-Date).AddSeconds($Seconds)
  do {
    Start-Sleep -Milliseconds 500
    $taskText = if (Test-Path -LiteralPath $taskLog) { Get-Content -LiteralPath $taskLog -Raw } else { '' }
    if ($taskText -match 'Scope: first window rows=' -or
        $taskText -match 'Native failed|Plugin callback threw' -or
        $taskProcess.HasExited) { break }
  } while ((Get-Date) -lt $taskDeadline)
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id }
  }
}

if (!(Test-Path -LiteralPath $taskLog)) { throw "Loader log missing: $taskLog" }
$taskLines = Get-Content -LiteralPath $taskLog
$taskText = $taskLines -join "`n"
$taskFatal = $taskLines | Where-Object { $_ -match 'Native failed|Plugin callback threw|ui 页面异常|回调异常' }
if ($taskFatal) { $taskFatal | Select-Object -First 8; throw 'The scope panel reported an error' }

foreach ($taskExpected in @(
    'Scope: registered board panel',
    'Scope: panel first drawn on frame',
    'Scope: armed on ',
    'Scope: first window rows=')) {
  if (!$taskText.Contains($taskExpected)) {
    $taskLines | Where-Object { $_ -match 'Scope|UI slot registered' } | Select-Object -Last 12
    throw "Missing evidence: $taskExpected"
  }
}
if (!($taskText -match 'UI slot registered: scope')) {
  throw 'The board panel was not registered with the host'
}

# The lanes: the panel has to have found wires on the board through
# tc.sim.channel, not reported zero.
$taskArmed = $taskLines | Where-Object { $_ -match 'Scope: armed on (\d+) wire\(s\)' } | Select-Object -Last 1
if (!$taskArmed) { throw 'The panel never reported its channel set' }
$null = $taskArmed -match 'Scope: armed on (\d+) wire\(s\)'
$taskLanes = [int]$Matches[1]
if ($taskLanes -lt 1) { throw 'The panel resolved no wire channels' }

# The window: a full ring of consecutive cycles from the level's own run.
$taskWindow = $taskLines | Where-Object { $_ -match 'Scope: first window rows=' } | Select-Object -Last 1
if (!($taskWindow -match 'rows=(\d+) gaps=(\d+) injected=(\d+) lanes=(\d+)')) {
  throw "The window line could not be read: $taskWindow"
}
$taskRows = [int]$Matches[1]
$taskGaps = [int]$Matches[2]
$taskInjected = [int]$Matches[3]
if ($taskRows -lt 8) { throw "The first captured window only held $taskRows row(s)" }
if ($taskGaps -ne 0) { throw "The first captured window has $taskGaps gap(s): the tick missed cycles" }
if ($taskInjected -ne 1) {
  throw 'The running program carries no per-cycle tick, so the window cannot be per-cycle'
}

"Scope panel: $taskLanes wire lane(s); first window rows=$taskRows gaps=$taskGaps " +
"injected=$taskInjected"
"PASS scope panel: the board panel resolved its lanes from the board's wires through " +
"tc.sim.channel, the loader's per-cycle tick filled the ring with no gaps while the level ran, " +
"and the host registered the panel on the board"
"Sandbox: $Sandbox"
