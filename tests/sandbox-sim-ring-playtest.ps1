# Does the sandbox simulator run on a board the game cannot compile?
#
# The injected schematic is the *closed* three-NAND ring (as a player would draw
# it, build/sandbox_sim_ring_closed_board.data): the records contain the loop, so
# our netlist builder sees a ring, while the game's own compiler refuses it.  The
# probe steps a few cycles so there is something to observe, and the simulator
# under test binds a netlist to that board and publishes into its state bytes.
#
# What this case answers, one way or the other:
#   * can a cyclic board be driven at all while the game owns the cycle counter?
#   * does the built netlist really contain the loop (three wires, three gates)?
param([switch]$Invert, [int]$FreeRun = 2, [switch]$OwnCycle, [switch]$NoSeed,
      [switch]$NoUnblock,
      [int]$StepsPerCycle = 1, [int]$GateDelay = 8,
      [int]$InteractiveLevel = -1,
      [string]$BoardFile = '',
      [ValidateSet('ring','latch','clock','edge','switch')] [string]$Circuit = 'ring')
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskSeconds = if ($env:TC_SMOKE_SECONDS) { [int]$env:TC_SMOKE_SECONDS } else { 35 }
$taskTest = Join-Path $taskRepo ('build\sandbox-sim-ring-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskSimData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.sandbox-sim'
$taskSchema = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default\schematics\architecture\Default'
New-Item -ItemType Directory -Force $taskRoot, $taskProfile, (Join-Path $taskRoot 'mods'), $taskSimData, $taskSchema | Out-Null
foreach ($taskDir in @('asset', 'campaign', 'translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll', 'soft_oal.dll', 'steam_api64.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\dev.enter-board.mod') -Destination (Join-Path $taskRoot 'mods\dev.enter-board.mod')
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\dev.sandbox-sim.mod') -Destination (Join-Path $taskRoot 'mods\dev.sandbox-sim.mod')
$taskTakeCycle = $OwnCycle -or $FreeRun -gt 0
$taskSeed = !$NoSeed -and $FreeRun -gt 0 -and $Circuit -eq 'ring'
"enabled=1`ninvert=$(if ($Invert) { 1 } else { 0 })`ntrace=1`ngate_delay=$GateDelay`nfree_run=$FreeRun`nown_cycle=$(if ($taskTakeCycle) { 1 } else { 0 })`nstartup_seed=$(if ($taskSeed) { 1 } else { 0 })`nunblock=$(if ($NoUnblock) { 0 } else { 1 })`nsteps_per_cycle=$StepsPerCycle`ninteractive_level=$InteractiveLevel`n" | Set-Content -LiteralPath (Join-Path $taskSimData 'sandbox-sim.txt') -Encoding ascii
$taskBoardName = switch ($Circuit) {
  'latch' { 'sandbox_sim_latch_board.data' }
  'clock' { 'sandbox_sim_clock_board.data' }
  'edge'  { 'sandbox_sim_edge_detector_board.data' }
  'switch' { 'sandbox_sim_switch_board.data' }
  default { 'sandbox_sim_ring_closed_board.data' }
}
$taskBoard = if ($BoardFile) { $BoardFile } else { Join-Path $taskRepo (Join-Path 'build' $taskBoardName) }
if (!(Test-Path -LiteralPath $taskBoard)) { throw "Missing sandbox simulator board: $taskBoard (run tools/sandbox-sim-boards.py)" }
Copy-Item -LiteralPath $taskBoard -Destination (Join-Path $taskSchema 'circuit.data')
$taskMods = @('dev.enter-board', 'dev.sandbox-sim')
if ($Circuit -eq 'clock' -or $Circuit -eq 'edge' -or $Circuit -eq 'switch') {
  Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\local.clock.mod') -Destination (Join-Path $taskRoot 'mods\local.clock.mod')
  $taskMods += 'local.clock'
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskMods
if ($LASTEXITCODE) { throw 'Package apply failed' }

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds $taskSeconds
  if (!$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
}

$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskReport = Join-Path $taskSimData 'sandbox-sim-report.txt'
if (Test-Path -LiteralPath $taskReport) { Get-Content -LiteralPath $taskReport -Raw }
Write-Output '--- loader log (sandbox-sim / compile / ring) ---'
Select-String -LiteralPath $taskLog -Pattern 'sandbox-sim|compile|ring|cycle|not compil' |
  Select-Object -Last 20 | ForEach-Object { $_.Line }
if ($FreeRun -gt 0) {
  # Our own clock has to move on a board the game refuses to advance.
  if (!(Test-Path -LiteralPath $taskReport)) {
    throw "The simulator wrote no report at $taskReport; loader log:`n" + (Get-Content -LiteralPath $taskLog -Tail 8 | Out-String)
  }
  $taskText = Get-Content -LiteralPath $taskReport -Raw
  if ($taskText -notmatch 'cycles_run=[1-9]\d*') {
    throw "free_run did not advance the simulator on the cyclic board: $taskText"
  }
  if ($Circuit -eq 'ring' -and $taskText -notmatch 'nets=6 devices=6 sources=0 observed=0') {
      throw "the cyclic board did not retain its three NANDs and three constants: $taskText"
  }
  if ($Circuit -eq 'latch' -and $taskText -notmatch 'nets=4 devices=5 sources=0 observed=0') {
      throw "the latch board did not retain its two NANDs and three constants: $taskText"
  }
  if ($Circuit -eq 'clock' -and $taskText -notmatch 'nets=2 devices=2 sources=0 observed=0') {
      throw "the clock board did not retain its host clock and two NOT gates: $taskText"
  }
  if ($taskSeed -and $Circuit -eq 'ring' -and $taskText -notmatch 'seeded_nets=[1-9]\d*') {
    throw "the explicit startup seed was not applied after initial evaluation: $taskText"
  }
  # Rendering fix (2026-09-25): the board paints a wire from
  # simulation_state[get_state_index(wire)], so a wire the game never allocated
  # (it resolves to the shared placeholder byte 1) would show a colour that is
  # not its own value.  Every wire must end up with a private, allocated index,
  # which shows up here as "no published slot below 256".
  if ($taskText -notmatch 'patched_wires=\d+') {
    throw "the simulator did not report how many wire state indices it owns: $taskText"
  }
  foreach ($taskNetLine in [regex]::Matches($taskText, 'net(\d+) value=\d source=[01] slots=([^\r\n]*)')) {
    foreach ($taskSlot in [regex]::Matches($taskNetLine.Groups[2].Value, '(\d+):')) {
      if ([int]$taskSlot.Groups[1].Value -lt 256) {
        throw ("net" + $taskNetLine.Groups[1].Value + " is still published to the shared placeholder byte " +
          $taskSlot.Groups[1].Value + ": " + $taskText)
      }
    }
  }
  $taskBindShapes = @(Select-String -LiteralPath $taskLog -Pattern 'bound board -> (nets=\d+ devices=\d+)' |
    ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -Unique)
  if ($taskBindShapes.Count -gt 1) {
    throw "the board's wire table changed shape mid-session: $($taskBindShapes -join ' / ')"
  }
  $taskRows = [regex]::Matches($taskText, 'history cycle=\d+(.*)')
  if ($taskRows.Count -lt 2) { throw "no cycle history to compare: $taskText" }
  # Sub-cycle publishing (2026-09-25): the edge detector's AND output is high
  # for one gate delay (8 ticks with the old scale, one unit with --GateDelay
  # 1024) inside an 8192-tick cycle.  With one publish per cycle that value can
  # never be observed; with `steps_per_cycle` it must appear in the published
  # rows - that is the whole point of the feature, so it is asserted, not printed.
  if ($Circuit -eq 'edge' -and $StepsPerCycle -gt 1) {
    $taskPulseRows = @($taskRows | Where-Object { $_.Groups[1].Value -match 'n2=1' })
    # The pulse is one gate delay wide.  A sample boundary lands strictly inside
    # it only when the sub-step is smaller than the pulse, so one sub-step per
    # gate delay (steps_per_cycle=8 with gate_delay=1024) catches it by a hair
    # while half that (16) catches it twice - the case asks for the stable
    # configuration, and the report keeps four cycles of rows so the pulse rows
    # cannot be truncated away.
    $taskWantRows = if ($StepsPerCycle -ge 16) { 2 } else { 1 }
    if ($taskPulseRows.Count -lt $taskWantRows) {
      throw "the edge pulse never reached the published rows: $taskText"
    }
    "sub-cycle publishing: the AND pulse is visible in $($taskPulseRows.Count) of $($taskRows.Count) published rows (want at least $taskWantRows)"
  }
  # The interactive switch (2026-09-25): its level lives in the instance's own
  # configuration, and the game's program - which used to copy it into a wire
  # slot - is suppressed here.  `-InteractiveLevel` performs the same write a
  # click performs (config write + instance refresh, see the Mod's
  # `interactive_level=` diagnostic), and the simulator must then drive its net
  # with that level.  Board: switch -> NOT -> NOT, so net0 is the switch's net.
  if ($Circuit -eq 'switch' -and $InteractiveLevel -ge 0) {
    if ($taskText -notmatch ("net0 value=" + ($InteractiveLevel -band 1) + " source=1")) {
      throw ("the switch's net was not driven by its configured level " + $InteractiveLevel + "; " + $taskText)
    }
    $taskWantNot = if (($InteractiveLevel -band 1) -eq 1) { 0 } else { 1 }
    if ($taskText -notmatch ("net1 value=" + $taskWantNot)) {
      throw ("the gate behind the switch did not follow it (want " + $taskWantNot + "); " + $taskText)
    }
    "interactive switch: configured level $InteractiveLevel reaches the engine as net0=$($InteractiveLevel -band 1) and net1=$taskWantNot"
  }
  if ($taskSeed -and $Circuit -eq 'ring') {
    $taskEvents = [regex]::Matches($taskText, 'event t=(\d+) net=(\d+) value=([01]) initial=0') |
      ForEach-Object { [pscustomobject]@{ Time = [int64]$_.Groups[1].Value; Net = [int]$_.Groups[2].Value } }
    $taskOscillator = $taskEvents | Group-Object Net | Where-Object Count -GE 4 | Select-Object -First 1
    if (!$taskOscillator) { throw "no ring net has four timed transitions: $taskText" }
    $taskTimes = @($taskOscillator.Group | Sort-Object Time -Unique | ForEach-Object Time)
    $taskDeltas = for ($taskAt = 1; $taskAt -lt $taskTimes.Count; ++$taskAt) {
      $taskTimes[$taskAt] - $taskTimes[$taskAt - 1]
    }
    $taskBadDelta = @($taskDeltas | Where-Object { $_ -ne 24 })
    if ($taskBadDelta.Count) {
      throw "ring transitions were not 3 stages * 8 ticks apart; deltas=$($taskDeltas -join ','): $taskText"
    }
    "seeded ring transition interval: 24 ticks (3 stages * 8 ticks); full value period: 48 ticks"
  }
  if ($Circuit -eq 'latch') {
    # Each row carries its engine time (`t=`), so the settle comparison looks at
    # the net values only - otherwise every row is "different" by timestamp.
    $taskHistory = @($taskRows | ForEach-Object { ($_.Groups[1].Value -replace '\s+t=\d+', '').Trim() })
    $taskTail = @($taskHistory | Select-Object -Last 8 | Select-Object -Unique)
    if ($taskTail.Count -ne 1) { throw "the NAND latch did not settle: $($taskHistory -join '; '): $taskText" }
    $taskKnown = [regex]::Matches($taskTail[0], 'n\d+=[01]').Count
    if ($taskKnown -lt 4) { throw "the NAND latch retained unknown nets: $($taskTail[0]): $taskText" }
    "two-NAND latch settled and held: $($taskTail[0])"
  }
  if ($Circuit -eq 'clock') {
    $taskEvents = [regex]::Matches($taskText, 'event t=(\d+) net=(\d+) value=([01]) initial=0') |
      ForEach-Object { [pscustomobject]@{ Time = [int64]$_.Groups[1].Value; Net = [int]$_.Groups[2].Value } }
    $taskGroups = @($taskEvents | Group-Object Net | Where-Object Count -GE 4)
    if ($taskGroups.Count -lt 2) { throw "clock and delayed output did not both toggle: $taskText" }
    $taskResidues = @($taskGroups | ForEach-Object {
      ($_.Group | Select-Object -Last 1).Time % 8192
    } | Sort-Object -Unique)
    if ($taskResidues.Count -ne 2 -or $taskResidues[1] - $taskResidues[0] -ne 8) {
      throw "NOT output was not 8 ticks behind the clock; residues=$($taskResidues -join ','): $taskText"
    }
    "clock and board output toggle every cycle with an 8-tick NOT delay"
  }
  "free_run advanced the simulator $($taskRows.Count) cycles on a board the game will not advance"
}
"Sandbox: $taskTest"
