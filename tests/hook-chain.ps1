# Loader-owned hook chains: ordering, argument edits, swallowing and the
# chain-point guard, driven offline by tests/hook-chain.cpp (which plays the part
# of the game with fake sim_do / sim_get_cycle / load_level symbols).
#
# The probe packages are built by build.ps1 (build\hook-chain-probe\*.dll and
# dist\dev.hook-chain-*.mod).  Each mode stages its own fixture, because a plugin
# can only join a chain while the loader is loading it.
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskGame=Split-Path $taskRepo
$taskEngine=Join-Path $taskGame 'tc_game_engine.dll'
if (!(Test-Path $taskEngine)) {$taskEngine=Join-Path $taskGame 'game_engine.dll'}
$taskProbes=@{
  chain=@(@{dll='a';id='dev.hook-chain-a'},@{dll='b';id='dev.hook-chain-b'})
  skip=@(@{dll='skip';id='dev.hook-chain-skip'})
  raw=@(@{dll='raw';id='dev.hook-chain-raw'})
  events=@(@{dll='events';id='dev.hook-chain-events'})
  crash=@(@{dll='crash';id='dev.hook-chain-crash'})
}
foreach($taskMode in @('chain','skip','raw','events','crash')) {
  $taskFixture=Join-Path $taskRepo ('build\hook-chain-test-'+$taskMode+'-'+[guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force (Join-Path $taskFixture 'mods') | Out-Null
  Copy-Item (Join-Path $taskRepo 'build\hook-chain-host.exe') (Join-Path $taskFixture 'Turing Complete.exe')
  Copy-Item (Join-Path $taskGame 'compile.dll') $taskFixture
  foreach($taskProbe in $taskProbes[$taskMode]) {
    $taskStage=Join-Path $taskFixture ('src-'+$taskProbe.id)
    New-Item -ItemType Directory -Force (Join-Path $taskStage 'native') | Out-Null
    Copy-Item (Join-Path $taskRepo ('build\hook-chain-probe\'+$taskProbe.dll+'.dll')) (Join-Path $taskStage 'native\hook-chain-probe.dll')
    ('{"format":2,"id":"'+$taskProbe.id+'","name":"Hook chain probe","version":"0.1.0","capabilities":["log","symbol","hook","symbol_alias","hook_chain","events"],"native":{"api":1,"entry":"native/hook-chain-probe.dll"}}') | Set-Content (Join-Path $taskStage 'mod.json') -Encoding ascii
    & (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskStage -Output (Join-Path $taskFixture ('mods\'+$taskProbe.id+'.mod')) | Out-Null
  }
  $taskResult=& (Join-Path $taskFixture 'Turing Complete.exe') $taskEngine $taskMode
  $taskExit=$LASTEXITCODE
  # The crash mode ends in a deliberate fault, so a non-zero exit is expected
  # there; every other mode has to exit 0.
  if($taskExit -and $taskMode -ne 'crash'){throw "Hook chain $taskMode test failed: $taskResult"}
  $taskText=$taskResult -join "`n"
  if($taskMode -eq 'chain') {
    # The loader, not the plugin, owns the detour: the install line is the proof
    # that one detour serves all three links.
    if($taskText -notmatch 'Hook chain sim\.do installed with 3 link\(s\)') {throw 'The loader did not install one sim.do chain for three links'}
  }
  if($taskMode -eq 'skip' -and $taskText -notmatch 'swallow=1') {throw 'The skipping link never ran'}
  if($taskMode -eq 'raw') {
    if($taskText -notmatch 'raw plain-hook ok=1') {throw 'A plain target could not be hooked raw'}
    if($taskText -notmatch 'raw sim\.do ok=0') {throw 'A raw hook on a chain point was accepted'}
  }
  if($taskMode -eq 'events') {
    foreach($taskNeedle in @('Event source level\.load: ok','Event source sim\.do: ok','Event source scene\.change armed','Event source save armed')) {
      if($taskText -notmatch $taskNeedle) {throw "The loader did not arm an event source: $taskNeedle"}
    }
  }
  if($taskMode -eq 'crash') {
    $taskFault=Join-Path $taskFixture 'tc-modloader-data\fault.log'
    if(!(Test-Path -LiteralPath $taskFault)) {throw 'The fault journal was not written'}
    $taskFaultText=Get-Content -LiteralPath $taskFault -Raw
    if($taskFaultText -notmatch 'dev\.hook-chain-crash') {throw "The fault journal does not name the plugin: $taskFaultText"}
    if($taskFaultText -notmatch 'sim\.do') {throw "The fault journal does not name the hook point: $taskFaultText"}
    if($taskFaultText -notmatch 'access violation') {throw "The fault journal does not name the fault: $taskFaultText"}
    if($taskExit -eq 0) {
      "NOTE: the fault was survived - containment is active, re-evaluate src/fault_guard.hpp"
    } else {
      "PASS fault journal: the fault killed the process as it would without the loader, and fault.log named the plugin, the hook point and the reason: $taskFaultText"
    }
  }
  $taskResult
}
