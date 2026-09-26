# The sandbox simulator in a real session (docs/PLAN-sandbox-simulator.md, S2).
#
# The enter-board driver opens the real sandbox with an AND board injected into
# its saved architecture, and dev.sandbox-sim binds a netlist to that board and
# publishes its own values into the state array on its own cycle.  With
# `invert=1` the values it publishes are the opposite of the ones it computed, so
# the report proves the bytes carry what *we* wrote rather than what the game's
# own program wrote a moment earlier.
param([switch]$Invert, [switch]$Disabled)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskSeconds = if ($env:TC_SMOKE_SECONDS) { [int]$env:TC_SMOKE_SECONDS } else { 40 }
foreach ($taskRequired in @('dist\dev.enter-board.mod', 'dist\dev.sandbox-sim.mod', 'dist\tc-loader.dll')) {
  if (!(Test-Path -LiteralPath (Join-Path $taskRepo $taskRequired))) {
    throw "Missing $taskRequired; run build.ps1 first"
  }
}
$taskTest = Join-Path $taskRepo ('build\sandbox-sim-playtest-' + [guid]::NewGuid().ToString('N'))
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
# The switch is a file (S2 keeps it out of the settings page until M3): with it
# absent the Mod does nothing, which is the default this test also relies on.
"enabled=$(if ($Disabled) { 0 } else { 1 })`ninvert=$(if ($Invert) { 1 } else { 0 })`ngate_delay=8`nfree_run=2`nown_cycle=1`n" | Set-Content -LiteralPath (Join-Path $taskSimData 'sandbox-sim.txt') -Encoding ascii
$taskSolution = Join-Path $taskRepo 'build\and2_solution_builtin.data'
if (!(Test-Path -LiteralPath $taskSolution)) { throw 'Missing build\and2_solution_builtin.data; run build.ps1 first' }
Copy-Item -LiteralPath $taskSolution -Destination (Join-Path $taskSchema 'circuit.data')
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.enter-board dev.sandbox-sim
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

$taskReport = Join-Path $taskSimData 'sandbox-sim-report.txt'
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
if ($Disabled) {
  $taskLogText = Get-Content -LiteralPath $taskLog -Raw
  if ($taskLogText -notmatch '\[dev\.sandbox-sim\] sandbox-sim: disabled; no hooks or callbacks installed') {
    throw "disabled mode did not take the zero-hook path: $taskLogText"
  }
  if ($taskLogText -match '\[dev\.sandbox-sim\] Hook chain|the cycle counter is ours') {
    throw "disabled mode installed a behavioural hook: $taskLogText"
  }
  if (Test-Path -LiteralPath $taskReport) {
    throw "disabled mode unexpectedly ran a frame callback and wrote $taskReport"
  }
  'Disabled mode installed no hooks or frame callback and wrote no state report'
  "Sandbox: $taskTest"
  return
}
if (!(Test-Path -LiteralPath $taskReport)) {
  throw "The sandbox simulator wrote no report; loader log tail:`n" + (Get-Content $taskLog -Tail 12 | Out-String)
}
$taskText = Get-Content -LiteralPath $taskReport -Raw
Write-Output $taskText
if ($taskText -notmatch 'bound=1 sandbox=1') { throw "The simulator did not bind a sandbox netlist: $taskText" }
if ($taskText -notmatch 'netlist|nets=\d+ devices=\d+ sources=\d+') { throw "No netlist summary in the report: $taskText" }
if ($taskText -notmatch 'published=[1-9]\d*') { throw "The simulator published nothing: $taskText" }
if ($taskText -notmatch 'notes=\d+|note: ' -and $taskText -notmatch 'nets=\d+ devices=\d+ sources=\d+ observed=\d+ cycles=\d+ published=') {
  throw "Unexpected report shape: $taskText"
}
if ($Invert) {
  # Every net we wrote must read back the inverted value: the board bytes are
  # ours, not the program's.
  $taskNets = [regex]::Matches($taskText, 'net(\d+) value=(\d) source=([01]) slots=([^\r\n]*)')
  if (!$taskNets.Count) { throw "No net lines to check: $taskText" }
  foreach ($taskNet in $taskNets) {
    $taskExpected = if ($taskNet.Groups[3].Value -eq '1') {
      $taskNet.Groups[2].Value
    } else {
      if ($taskNet.Groups[2].Value -eq '1') { '0' } else { '1' }
    }
    foreach ($taskSlot in [regex]::Matches($taskNet.Groups[4].Value, '(\d+):(\d)')) {
      if ($taskSlot.Groups[2].Value -ne $taskExpected) {
        throw "net$($taskNet.Groups[1].Value) slot $($taskSlot.Groups[1].Value) reads $($taskSlot.Groups[2].Value) but expected $taskExpected"
      }
    }
  }
  "The state bytes carry the value the simulator published ($($taskNets.Count) nets checked)"
}
"Sandbox: $taskTest"
