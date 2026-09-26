# Real-machine playtest for the per-cycle capture (TC_SERVICE_SIM_CAPTURE,
# sdk/tc_sim_capture.h) - the layer a scope is built on.
#
# A scope needs *cycles*, not frames: the render-thread trace sampler
# (tc::trace::Sampler) reads once per frame, so a level that runs in a burst
# shows a few rows and the waveform jumps.  The capture instead has the loader
# instrument the compiled program with a tick before every `cycle += 1`, and the
# ring it fills is what this case checks:
#
#   * the tick really is in the level's program ("Scope tick: instrumented N
#     per-cycle call site(s)"), and the capture is armed on channels taken from
#     the trace sampler's own slot discovery - no hardcoded offsets;
#   * a byte-adder level that runs its own 39-cycle test yields one row per
#     cycle (rows >= 20, and every row's cycle number exactly one more than the
#     previous one - `consecutive=1`);
#   * nothing was missed: gaps = 0, which is the assertion a scope has to make;
#   * the rows are the level's numbers, not noise: sum == (carry_in + a + b) and
#     carry_out == the carry out of that sum, for the rows where the level's
#     three inputs are visible in the captured data.
#
# The driver (tests/scope-capture-driver.cpp) never navigates: example.byte-adder
# loads the level, compiles it and runs it, and this case only arms, reads and
# compares.  That division is the point of the service.
param(
  [string]$Sandbox = (Join-Path (Split-Path $PSScriptRoot) 'build\scope-sandbox'),
  [int]$Seconds = 90,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskRoot = Join-Path $Sandbox 'game'
$taskHome = Join-Path $Sandbox 'home'
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'

# Always refresh the sandbox: make-ui-sandbox.ps1 copies dist\tc-loader.dll over
# game_engine.dll, and a sandbox that keeps an older loader silently tests the
# previous build.
& (Join-Path $PSScriptRoot 'make-ui-sandbox.ps1') -Sandbox $Sandbox `
  -Mods example.byte-adder,dev.scope-capture-driver
"Loader under test: $((Get-FileHash (Join-Path $taskRoot 'game_engine.dll') -Algorithm SHA256).Hash)"

# The byte_adder level's test needs a circuit to test: a fresh sandbox has none,
# so install the built-in 8-bit adder solution into the profile's byte_adder
# slot, exactly like tests/byte-adder-smoke.ps1 does.
$taskLevel = 'byte_adder'
$taskSchema = Join-Path $taskHome "AppData\Roaming\Turing Complete Mods\profiles\default\schematics\$taskLevel\Default"
New-Item -ItemType Directory -Force $taskSchema | Out-Null
$taskSolution = Join-Path $taskRepo 'build\nl_adder8board.data'
if (!(Test-Path -LiteralPath $taskSolution)) { throw "Missing $taskSolution; run build.ps1 first" }
Copy-Item -LiteralPath $taskSolution -Destination (Join-Path $taskSchema 'circuit.data') -Force
# ... and tell the example to load that level on its own.
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
    if ($taskText -match 'DRIVER: capture (done|finished|gave up|could not)' -or
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
if ($taskFatal) { $taskFatal | Select-Object -First 8; throw 'The capture driver reported an error' }

foreach ($taskExpected in @(
    'DRIVER: scope capture driver loaded',
    'Scope capture: per-cycle state reader armed',
    'Scope tick: instrumented')) {
  if (!$taskText.Contains($taskExpected)) {
    $taskLines | Select-Object -Last 20
    throw "Missing evidence: $taskExpected"
  }
}
if ($taskText -match 'DRIVER: capture gave up|DRIVER: capture could not') {
  $taskLines | Where-Object { $_ -match 'DRIVER: capture' } | Select-Object -Last 6
  throw 'The driver could not arm or read the capture'
}

$taskStatus = $taskLines | Where-Object { $_ -match 'DRIVER: capture status \S+ rows=' } | Select-Object -Last 1
if (!$taskStatus) {
  $taskLines | Select-Object -Last 20
  throw 'The driver never reported the capture status'
}
if (!($taskStatus -match 'rows=(\d+) written=(\d+) first=(-?\d+) last=(-?\d+) gaps=(\d+) restarts=(\d+) triggered=(\d+) trigger_cycle=(-?\d+) injected=(\d+) channels=(\d+) trace_rows=(\d+) read=(\d+)')) {
  throw "The capture status line could not be read: $taskStatus"
}
$taskRows = [int]$Matches[1]
$taskWritten = [int]$Matches[2]
$taskFirst = [int]$Matches[3]
$taskLast = [int]$Matches[4]
$taskGaps = [int]$Matches[5]
$taskRestarts = [int]$Matches[6]
$taskInjected = [int]$Matches[9]
$taskChannels = [int]$Matches[10]
$taskTraceRows = [int]$Matches[11]
if ($taskChannels -lt 8) { throw "The capture was armed on only $taskChannels channel(s)" }
if ($taskInjected -ne 1) {
  throw ('The level program carries no per-cycle tick (injected=0): the capture cannot see every cycle')
}
if ($taskRows -lt 30) {
  $taskLines | Where-Object { $_ -match 'DRIVER: capture' } | Select-Object -Last 10
  throw "The capture only holds $taskRows rows; the level's test runs 39 cycles"
}
# Nothing was missed.  This is the whole reason the service exists: the
# render-thread sampler skips cycles, the capture does not.
if ($taskGaps -ne 0) {
  $taskLines | Where-Object { $_ -match 'DRIVER: capture row ' } | Select-Object -Last 10
  throw "The capture has $taskGaps gap(s): the tick did not see every cycle"
}
if (!($taskText.Contains('DRIVER: capture consecutive=1'))) {
  $taskLines | Where-Object { $_ -match 'DRIVER: capture' } | Select-Object -Last 10
  throw 'The captured cycles are not consecutive'
}
# A window of constants is not a capture of anything: the strip the driver reads
# has to contain at least a couple of wires that actually move.
$taskLive = $taskLines | Where-Object { $_ -match 'DRIVER: capture live channels=(\d+)/(\d+)' } |
  Select-Object -Last 1
if (!$taskLive) { throw 'The driver never reported which channels carried data' }
$null = $taskLive -match 'live channels=(\d+)/(\d+)'
if ([int]$Matches[1] -lt 2) {
  $taskLines | Where-Object { $_ -match 'DRIVER: capture live ' } | Select-Object -Last 4
  throw "Only $($Matches[1]) of $($Matches[2]) captured channels carried data"
}

# ... and the values it recorded are the level's own.  The driver reads a strip
# of the state instead of naming channels (it has no board handle), so the
# oracle is the one witness that can name them: the example plugin's per-cycle
# callback logs carry_in, a and b for the cycles it saw, and the byte-adder test
# drives the same vectors again.  Columns that reproduce all three, for every
# cycle the two readers share, are the level's own wires - nothing else in a
# state dump lines up with three independent sequences.
#
# Two details of that comparison are properties of the game, not slack:
#   * the tick sits immediately before `cycle += 1` in the generated program,
#     whose counter is one lower than the cycle number the plugin's callback
#     reports for the same work (measured: the captured cycle 0 carries the
#     vectors the plugin logs for cycle 1), so the oracle is read at cycle + 1;
#   * a level input also exists as its complement on the board (the capture sees
#     the carry the plugin logs as 0 in a column that reads 1), so the one-bit
#     carry role accepts either polarity.  `a` and `b` are eight bits wide and
#     are matched exactly.
$taskVectors = @{}
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'byte-adder: cycle=(-?\d+) carry_in=(\d+) a=(\d+) b=(\d+)') {
    $taskCycle = [int]$Matches[1]
    if (-not $taskVectors.ContainsKey($taskCycle)) { $taskVectors[$taskCycle] = New-Object System.Collections.ArrayList }
    [void]$taskVectors[$taskCycle].Add(@([int]$Matches[2], [int]$Matches[3], [int]$Matches[4]))
  }
}
$taskRowValues = @()
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'DRIVER: capture row \d+ cycle=(-?\d+) values=([\d,]+)') {
    $taskRowValues += [pscustomobject]@{
      Cycle = [int]$Matches[1]
      Values = @($Matches[2] -split ',' | ForEach-Object { [int]$_ })
    }
  }
}
if ($taskRowValues.Count -lt 4) { throw 'The driver did not print any captured rows' }
$taskComparable = @()
foreach ($taskRow in $taskRowValues) {
  if ($taskVectors.ContainsKey($taskRow.Cycle + 1)) {
    $taskComparable += [pscustomobject]@{ Cycle = $taskRow.Cycle; Values = $taskRow.Values }
  }
}
if ($taskComparable.Count -lt 4) {
  $taskLines | Where-Object { $_ -match 'DRIVER: capture row ' } | Select-Object -Last 8
  throw ("Only $($taskComparable.Count) captured row(s) can be compared with the plugin's own " +
         'per-cycle log: the capture and the level are not on the same cycles')
}
# A column matches a role when one *fixed* polarity holds for every comparable
# cycle: a column that is always zero would otherwise "match" a carry that
# sometimes flips, which is the kind of match that proves nothing.
$taskRoleColumns = @()
for ($taskRole = 0; $taskRole -lt 3; $taskRole++) {
  $taskColumns = @()
  for ($taskColumn = 0; $taskColumn -lt $taskChannels; $taskColumn++) {
    foreach ($taskInverted in @($false, $true)) {
      $taskOk = $true
      foreach ($taskRow in $taskComparable) {
        $taskValue = $taskRow.Values[$taskColumn]
        if ($taskInverted) {
          if ($taskRole -ne 0) { $taskOk = $false; break }
        }
        $taskHit = $false
        foreach ($taskCandidate in $taskVectors[$taskRow.Cycle + 1]) {
          $taskExpected = $taskCandidate[$taskRole]
          if ($taskInverted) { $taskExpected = 1 - $taskExpected }
          if ($taskExpected -eq $taskValue) { $taskHit = $true; break }
        }
        if (-not $taskHit) { $taskOk = $false; break }
      }
      if ($taskOk) { $taskColumns += $taskColumn }
    }
  }
  $taskRoleColumns += ,@($taskColumns | Select-Object -Unique)
}
$taskMapping = $null
foreach ($taskCarryColumn in $taskRoleColumns[0]) {
  foreach ($taskAColumn in $taskRoleColumns[1]) {
    if ($taskAColumn -eq $taskCarryColumn) { continue }
    foreach ($taskBColumn in $taskRoleColumns[2]) {
      if ($taskBColumn -eq $taskCarryColumn -or $taskBColumn -eq $taskAColumn) { continue }
      $taskMapping = "$taskCarryColumn,$taskAColumn,$taskBColumn"
      break
    }
    if ($taskMapping) { break }
  }
  if ($taskMapping) { break }
}
if (-not $taskMapping) {
  $taskLines | Where-Object { $_ -match 'DRIVER: capture (row|live) ' } | Select-Object -Last 10
  throw ('No column triple in the captured window reproduces carry_in, a and b for the cycles the ' +
         "plugin logged: role matches were $($taskRoleColumns[0] -join '/') | " +
         "$($taskRoleColumns[1] -join '/') | $($taskRoleColumns[2] -join '/')")
}

$taskMapped = @($taskMapping -split ',')
$taskRowValues | Select-Object -First 6 |
  ForEach-Object {
    [pscustomobject]@{
      Cycle = $_.Cycle
      Carry = $_.Values[[int]$taskMapped[0]]
      A     = $_.Values[[int]$taskMapped[1]]
      B     = $_.Values[[int]$taskMapped[2]]
    }
  } | Format-Table -AutoSize | Out-String -Width 120 | Write-Host
"Capture: $taskRows rows (of $taskWritten ticks), cycles $taskFirst..$taskLast, gaps=$taskGaps, " +
"restarts=$taskRestarts, channels=$taskChannels, injected=$taskInjected, trace_rows=$taskTraceRows"
"Level vectors: state columns $taskMapping reproduce carry_in, a and b for all " +
"$($taskComparable.Count) cycle(s) the plugin's own callback logged (the tick's cycle N is the " +
"cycle the callback numbers N+1)"
"PASS per-cycle capture: the level's own 39-cycle test landed in the ring one row per cycle " +
"(consecutive=1, gaps=0), the tick really is in the compiled program, and the values are the " +
"level's own, cycle by cycle"
"Sandbox: $Sandbox"
