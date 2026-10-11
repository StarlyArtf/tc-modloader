# Development case for the two 0-pin component shapes: a source (0 inputs,
# 1 output) and a sink (1 input, 0 outputs).
#
# The loader can now encode both scaffolds and the bridge accepts them, but the
# game decides the rest: whether it imports a definition with no pins on one
# side, and whether the compiler keeps - let alone schedules - the one node
# those shapes have to run on (a driver with an unconnected input for a source,
# a driver with an unconnected output for a sink).
#
# The probe (tests/pin-shape-probe.cpp) reuses the byte-adder example's level
# runner and replaces its registration with the shape under test, so the run is
# the same as any other declarative component: a real level compiles it and its
# test drives the simulation.  What this script does is put the matching board
# fixture into that level and report what the log says.
#
#   -Shape source : build\nl_src0board.data  + or_gate  (the source drives 0 into an OR)
#   -Shape sink   : build\nl_sink0board.data + not_gate (the level keeps its own NOT path)
#
# -KeepRunning leaves the game up for a person; the default run is headless.
param(
  [ValidateSet('source','sink','wide9','wide2048','clocked')]
  [string]$Shape = 'source',
  [int]$Seconds = 50,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskTest = Join-Path $taskRepo ('build\pin-shape-' + $Shape + '-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.pin-shape-probe'
$taskLevel = if ($Shape -eq 'sink' -or $Shape -eq 'wide2048') { 'not_gate' }
             elseif ($Shape -eq 'clocked') { 'full_adder' }
             else { 'or_gate' }
$taskSchema = Join-Path $taskProfile ("AppData\Roaming\Turing Complete Mods\profiles\default\schematics\$taskLevel\Default")
$taskBoardName = switch ($Shape) {
  'source' { 'nl_src0board.data' }
  'sink'   { 'nl_sink0board.data' }
  'wide9'  { 'nl_wide9board.data' }
  'wide2048' { 'nl_wide2048board.data' }
  'clocked' { 'nl_adderboard.data' }
}
$taskBoard = Join-Path $taskRepo ('build\' + $taskBoardName)
if (!(Test-Path -LiteralPath $taskBoard)) { throw "Missing $taskBoard; run build.ps1 first" }

New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods'),$taskData,$taskSchema | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\dev.pin-shape-probe.mod') -Destination (Join-Path $taskRoot 'mods\dev.pin-shape-probe.mod')
Copy-Item -LiteralPath $taskBoard -Destination (Join-Path $taskSchema 'circuit.data')
Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value $taskLevel -Encoding ascii
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.pin-shape-probe
if($LASTEXITCODE){throw 'Pin shape probe apply failed'}

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskProcess = $null
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = (Get-Date).AddSeconds($Seconds)
  do {
    Start-Sleep -Milliseconds 500
    $taskText = if (Test-Path -LiteralPath (Join-Path $taskRoot 'tc-modloader-data\loader.log')) {
      Get-Content -LiteralPath (Join-Path $taskRoot 'tc-modloader-data\loader.log') -Raw
    } else { '' }
    # The instance walk (multi-instance, reset, destroy) runs after the level
    # runner reports, so the loop waits for its own marker.
    if ($taskText -match 'pin-shape: instances stage done' -or
        $taskText -match 'autotest finished' -and $Shape -ne 'wide9' -or
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

$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
if (!(Test-Path -LiteralPath $taskLog)) { throw "Missing loader log; inspect $taskTest" }
$taskLines = Get-Content -LiteralPath $taskLog
$taskText = $taskLines -join "`n"

# The measurement itself: everything the loader and the game said about the
# shape, printed so the research note quotes the real log rather than a summary.
$taskEvidence = $taskLines | Where-Object {
  $_ -match 'pin-shape:|Native logic:|byte-adder: autotest|Native failed'
}
$taskEvidence | Select-Object -First 40 | ForEach-Object { $_ }
if (!$taskText.Contains('pin-shape: source registration') -and
    !$taskText.Contains('pin-shape: sink registration') -and
    !$taskText.Contains('pin-shape: wide9 V2 registration') -and
    !$taskText.Contains('pin-shape: wide2048 V2 registration')) {
  throw "The probe never reached registration; inspect $taskLog"
}
if (!($taskText -match 'byte-adder: autotest finished')) {
  throw "The level runner did not finish; inspect $taskLog"
}

$taskPattern = switch ($Shape) {
  'source' { 'pin-shape: source registration' }
  'sink'   { 'pin-shape: sink registration' }
  'wide9'  { 'pin-shape: wide9 V2 registration' }
  'wide2048' { 'pin-shape: wide2048 V2 registration' }
  'clocked' { 'pin-shape: clocked V2 registration' }
}
$taskStatus = $taskLines | Where-Object { $_ -match $taskPattern } | Select-Object -Last 1
$null = $taskStatus -match '-> (-?\d+)'
$taskResult = [int]$Matches[1]
$taskCalls = @($taskLines | Where-Object { $_ -match ('pin-shape: ' + $Shape + ' cycle=') }).Count
$taskBound = @($taskLines | Where-Object { $_ -match 'Native logic: bound instance' }).Count

"Shape $Shape : registration=$taskResult, bound instances=$taskBound, callback cycle logs=$taskCalls"
if ($taskResult -ne 0) {
  throw ("The loader refused the $Shape shape (registration=$taskResult); the game's answer is in the " +
         "log above.  Record it in docs/research/custom-component-pins.md")
}
if ($taskBound -lt 1) {
  throw ("The $Shape instance was not bound to the bridge: its internal node count did not match the " +
         'scaffold, so the game compiled a different circuit than the encoder wrote (see the log)')
}
if ($taskCalls -lt 1) {
  throw "The $Shape callback never ran: the compiled program has no scheduled node for it"
}
if ($Shape -eq 'wide2048') {
  $taskCycleLine = $taskLines | Where-Object { $_ -match 'pin-shape: wide2048 cycle=' } | Select-Object -Last 1
  if (!($taskCycleLine -match 'inputs=(\d+) nonzero=(\d+) first=(\d+) last=(\d+)')) {
    throw "The wide2048 callback log could not be read: $taskCycleLine"
  }
  if ([int]$Matches[1] -ne 32) { throw "The staged callback saw $($Matches[1]) inputs, expected 32" }
  if ([int]$Matches[2] -ne 1) { throw "Expected exactly one driven wide input, saw $($Matches[2])" }
  if ([uint64]$Matches[3] -ne 0) { throw "The unconnected first word was $($Matches[3]), expected 0" }
  if ([uint64]$Matches[4] -ne 0x0123456789abcdef) {
    throw "The word above bit 1984 was $($Matches[4]), expected 0x0123456789abcdef"
  }
  $taskNoted = $taskLines | Where-Object { $_ -match 'shape=32in/1out v2 state=0' } | Select-Object -First 1
  if (!$taskNoted) { throw 'The loader did not report the 32-input V2 shape' }
  "PASS wide2048 component: 32 x 64-bit inputs registered and the last staged word reached the callback intact"
}
if ($Shape -eq 'clocked') {
  # The clocked contract: exact visible dependency cones plus a disconnected
  # hidden sink that sees every input and advances state once per cycle.
  $taskNoted = $taskLines | Where-Object {
    $_ -match 'shape=3in/2out v3 state=4' -and $_ -match 'deps=0x7,0x7' -and
    $_ -match 'tick=hidden' } | Select-Object -First 1
  if (!$taskNoted) { throw 'The loader did not report exact clocked dependencies and the hidden tick' }
  $taskCycleLines = @($taskLines | Where-Object { $_ -match 'pin-shape: clocked cycle=' })
  $taskLast = $taskCycleLines | Select-Object -Last 1
  if (!($taskLast -match 'cycle=(\d+) calls=(\d+) state=(\d+) clk=(\d)')) {
    throw "The clocked callback log could not be read: $taskLast"
  }
  $taskCycle = [int]$Matches[1]; $taskCalls = [int]$Matches[2]
  $taskState = [int]$Matches[3]
  if ($taskCalls -ne $taskCycle + 1) {
    throw "The clocked callback ran $taskCalls time(s) by cycle $taskCycle; expected exactly one tick per cycle"
  }
  if ($taskState -ne $taskCalls) {
    throw "The clocked state advanced to $taskState after $taskCalls tick(s); a second call per cycle would double it"
  }
  if (!($taskCycleLines -match 'clk=1')) {
    throw "The hidden tick never saw input 0 (the clock) high; it must receive every input"
  }
  $taskVerdict = $taskLines | Where-Object { $_ -match 'byte-adder: autotest finished' } | Select-Object -Last 1
  if (!($taskVerdict -match 'verdict=1')) {
    throw "The full_adder level did not pass ($taskVerdict); the precise public outputs did not reach the board"
  }
  "PASS clocked component: one hidden tick per cycle, every input packed, precise public outputs reached the board"
}
if ($Shape -eq 'wide9') {
  # The V2 shape: nine pins through TCLogicIOV2, and a state size the definition
  # chose rather than the fixed eight words.
  $taskCycleLine = $taskLines | Where-Object { $_ -match 'pin-shape: wide9 cycle=' } | Select-Object -Last 1
  if (!($taskCycleLine -match 'inputs=(\d+)\[([\d,]+)\] state=(\d+)')) {
    $taskLines | Where-Object { $_ -match 'pin-shape|Native logic' } | Select-Object -Last 12
    throw "The wide9 callback log could not be read: $taskCycleLine"
  }
  $taskInputs = [int]$Matches[1]
  $taskValues = @($Matches[2] -split ',')
  $taskState = [int]$Matches[3]
  if ($taskInputs -ne 9) { throw "The V2 callback saw $taskInputs input(s), expected 9" }
  if ($taskValues.Count -ne 9) { throw "Only $($taskValues.Count) value(s) arrived for the nine inputs" }
  $taskNoted = $taskLines | Where-Object { $_ -match 'shape=9in/1out v2 state=2' } | Select-Object -First 1
  if (!$taskNoted) { throw 'The loader did not report the V2 shape with its own state size' }
  # The instance walk: create, a second instance placed at run time, per-instance
  # reset, delete, and the handle that outlived its instance.
  $taskRequired = @(
    @{Pattern='pin-shape: wide9 on_create instance=\d+';Why='lifecycle on_create for the first instance'},
    @{Pattern='pin-shape: instances enumerated: enum status=0 written=2 total=2';Why='both instances of the type enumerated'},
    @{Pattern='pin-shape: instances reset status=0 first_before=\d+ first_after=0 second_state=\d+';Why='reset touched only the target instance'},
    @{Pattern='pin-shape: storage info=0 read_before=0 write=0 read_after=0 info_after=0 schema=3 bytes=4 before=7 after=42 revision=2';Why='configuration was atomically replaced and survived reset'},
    @{Pattern='pin-shape: instances delete index=\d+';Why='the first instance was deleted from the board'},
    @{Pattern='pin-shape: wide9 on_destroy instance=\d+';Why='lifecycle on_destroy for the released instance'},
    @{Pattern='pin-shape: instances after delete: enum status=0 written=1 total=1';Why='only the second instance survives'},
    @{Pattern='pin-shape: instances old_handle=-4 forged_handle=-4';Why='stale handles are refused, not resolved'},
    # M2's placement half (2026-09-28): compile, resize, move and unbind are all
    # driven from the probe with the game's own paths, and the type's lifecycle
    # has to hear them.
    @{Pattern='pin-shape: wide9 on_compile instance=\d+';Why='TC_LOGIC_COMPILE for an instance the board compiled'},
    @{Pattern='pin-shape: geometry instance resize=0 \(half=3,2\)';Why='the per-instance footprint was accepted'},
    @{Pattern='pin-shape: wide9 on_resize instance=\d+ cell=-?\d+,-?\d+ rotation=\d+ half=3(\.0+)?,2(\.0+)?';Why='TC_LOGIC_RESIZE with the new footprint in the placement tail'},
    # Move and rotate are *attempted* here (the probe drags the component with
    # synthetic input and tries the rotate candidates), but the game does not act
    # on a synthetic board drag in this build - the same finding the hitbox
    # research recorded - so their real-machine gate is still open.  What the
    # probe writes about the attempt is a diagnostic, not an assertion.
    @{Pattern='pin-shape: moving instance at \d+,\d+ by two cells';Why='the probe attempted the move gesture'},
    @{Pattern='pin-shape: leaving the board for sandbox';Why='the probe left the board for a second level'},
    @{Pattern='pin-shape: wide9 on_unbind instance=\d+ phase=13';Why='TC_LOGIC_UNBIND when the board went away (the instance was not deleted)'}
  )
  foreach ($taskExpect in $taskRequired) {
    if (!($taskText -match $taskExpect.Pattern)) {
      $taskLines | Where-Object { $_ -match 'pin-shape: instances|wide9 on_' } | Select-Object -Last 12
      throw ("Missing instance evidence: {0} ({1})" -f $taskExpect.Pattern, $taskExpect.Why)
    }
  }
  $taskReset = $taskLines | Where-Object { $_ -match 'pin-shape: instances reset status=' } | Select-Object -Last 1
  $null = $taskReset -match 'second_state=(\d+)'
  if ([int]$Matches[1] -eq 0) { throw 'The second instance had no state of its own to protect' }
  "wide9 last cycle line: $taskCycleLine"
  "Instances: $($taskLines | Where-Object { $_ -match 'pin-shape: instances (before|after) place' } | Select-Object -Last 1)"
  "PASS wide9 component: tc.component.types registered a nine-pin V2 definition, the bridge bound it, " +
    "the callback ran per cycle with all nine inputs and its own two state words; a second instance was " +
    "placed and enumerated, storage configuration survived reset, and deleting the first one fired on_destroy " +
  "and left its handle refused as stale"
  "Sandbox: $taskTest"
  return
}
"PASS $Shape component: the game imported the 0-pin scaffold, the bridge bound its instance and the callback ran"
"Sandbox: $taskTest"
