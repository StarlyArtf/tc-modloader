# Runs the simulation-state mapping probe in an isolated sandbox.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskSeconds = if($env:TC_SMOKE_SECONDS) { [int]$env:TC_SMOKE_SECONDS } else { 25 }
$taskMod = Join-Path $taskRepo 'dist\dev.sim-state.mod'
if(!(Test-Path -LiteralPath $taskMod)) { throw 'Missing dist\dev.sim-state.mod; run build.ps1 first' }
$taskCustom = ($env:TC_SIM_STATE_CUSTOM -eq '1')
$taskLogic = $env:TC_SIM_STATE_LOGIC
# Extra packages to load next to the probe, by mod id: the real-machine check for
# a feature that has to be exercised while the simulation runs uses this to add
# example.cycle-guard (its sim.do chain link is idle on this 4-cycle level, so
# the probe's own assertions stay untouched).
# Assigned in two steps on purpose: `$x = if (...) { @(...) }` unrolls a
# single-element array to a string (measured), and splatting a bare string with
# @name then passes something unrelated - the CLI saw "-".
$taskExtras = @()
if($env:TC_SIM_STATE_EXTRA_MODS) { $taskExtras = @($env:TC_SIM_STATE_EXTRA_MODS -split ',' | Where-Object { $_ }) }

$taskTest = Join-Path $taskRepo ('build\sim-state-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.sim-state'
$taskSchema = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default\schematics\and_gate\Default'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods'),$taskData,(Join-Path $taskData 'fixtures'),$taskSchema | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath $taskMod -Destination (Join-Path $taskRoot 'mods\dev.sim-state.mod')
if($taskLogic) {
  Set-Content -LiteralPath (Join-Path $taskData 'logic.txt') -Value $taskLogic -Encoding ascii
}
$taskSolution = if($taskCustom) { Join-Path $taskRepo 'build\and2_solution.data' } else { Join-Path $taskRepo 'build\and2_solution_builtin.data' }
if(!(Test-Path -LiteralPath $taskSolution)) { throw "Missing sim-state solution: $taskSolution" }
Copy-Item -LiteralPath $taskSolution -Destination (Join-Path $taskSchema 'circuit.data')
if($taskCustom) {
  $taskFixture = Join-Path $taskRepo 'build\and2_component.data'
  if(!(Test-Path -LiteralPath $taskFixture)) { throw "Missing custom fixture: $taskFixture" }
  Copy-Item -LiteralPath $taskFixture -Destination (Join-Path $taskData 'fixtures\and2_component.data')
}
foreach($taskExtra in $taskExtras) {
  $taskExtraMod = Join-Path $taskRepo ('dist\'+$taskExtra+'.mod')
  if(!(Test-Path -LiteralPath $taskExtraMod)) { throw "Missing extra package: $taskExtraMod" }
  Copy-Item -LiteralPath $taskExtraMod -Destination (Join-Path $taskRoot ('mods\'+$taskExtra+'.mod'))
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.sim-state @taskExtras
if($LASTEXITCODE){throw 'Sim state package apply failed'}

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds $taskSeconds
  if(!$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
}

$taskOut = Join-Path $taskRepo 'build\state-map.txt'
if(Test-Path -LiteralPath (Join-Path $taskData 'state-map.txt')) {
  Copy-Item -LiteralPath (Join-Path $taskData 'state-map.txt') -Destination $taskOut -Force
  $taskMap = Get-Content -LiteralPath $taskOut -Raw
  $taskRequired = if($taskLogic -eq 'or') {
    @(
      'input_replay slot 0 seq=0,1,2,3,',
      'input_replay slot 8 seq=0,1,2,3,',
      'output_history slot 55 seq=0,1,1,1,',
      'output_history slot 64 seq=0,1,1,1,'
    )
  } else {
    @(
      'input_replay slot 0 seq=0,1,2,3,',
      'input_replay slot 8 seq=0,1,2,3,',
      'output_history slot 55 seq=0,0,0,1,',
      'output_history slot 64 seq=0,0,0,1,'
    )
  }
  foreach($taskPattern in $taskRequired) {
    if($taskMap -notmatch [regex]::Escape($taskPattern)) {
      throw "Sim state mapping assertion failed: missing /$taskPattern/ in $taskOut"
    }
  }
  if($taskLogic -eq 'or') {
    $taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
    if(!(Select-String -LiteralPath $taskLog -Pattern 'snapshot at cycle 3 .*test_state=2' -Quiet)) {
      throw 'Custom OR logic did not keep the test failed through cycle 3; inspect loader.log'
    }
  }
  Select-String -LiteralPath $taskOut -Pattern '^state_buffer=','^input_replay slot (0|8) ','^output_history slot (55|64) ','^input_replay changed_slots=','^output_history changed_slots=' |
    ForEach-Object { $_.Line }
  "Saved: $taskOut"
} else {
  "No state-map.txt; loader log:"; Get-Content (Join-Path $taskRoot 'tc-modloader-data\loader.log') -Tail 8
}
"Sandbox: $taskTest"
