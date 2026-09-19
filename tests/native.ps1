$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskGame=Split-Path $taskRepo
$taskEngine=Join-Path $taskGame 'tc_game_engine.dll'
if (!(Test-Path $taskEngine)) {$taskEngine=Join-Path $taskGame 'game_engine.dll'}
foreach($taskMode in @('normal','tamper','conflict','disabled')) {
 $taskFixture=Join-Path $taskRepo ('build\native-test-'+$taskMode+'-'+[guid]::NewGuid().ToString('N'))
 New-Item -ItemType Directory -Force (Join-Path $taskFixture 'mods') | Out-Null
 Copy-Item (Join-Path $taskRepo 'build\native-host.exe') (Join-Path $taskFixture 'Turing Complete.exe')
 # The runtime hashes the game's compile.dll to decide whether the native-logic
 # bridge applies; without it every plugin load aborts inside boot().
 Copy-Item (Join-Path $taskGame 'compile.dll') $taskFixture
 Copy-Item (Join-Path $taskRepo 'dist\example.cycle-guard.mod') (Join-Path $taskFixture 'mods\guard.mod')
 if($taskMode -eq 'conflict'){
  # Two packages that both want a raw hook on the same plain target: the first is
  # accepted, the second must be refused.  (The chain points are covered by
  # tests/hook-chain.ps1, where a raw hook is refused for a different reason.)
  foreach($taskId in @('z.conflict-a','z.conflict-b')) {
   $taskConflict=Join-Path $taskFixture ('conflict-source-'+$taskId)
   New-Item -ItemType Directory -Force (Join-Path $taskConflict 'native') | Out-Null
   ('{"format":2,"id":"'+$taskId+'","name":"Hook conflict probe","version":"0.1.0","capabilities":["log","symbol","hook"],"native":{"api":1,"entry":"native/hook-chain-probe.dll"}}') | Set-Content (Join-Path $taskConflict 'mod.json') -Encoding ascii
   Copy-Item (Join-Path $taskRepo 'build\hook-chain-probe\dup.dll') (Join-Path $taskConflict 'native\hook-chain-probe.dll')
   & (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskConflict -Output (Join-Path $taskFixture ('mods\'+$taskId+'.mod')) | Out-Null
  }
 }
 $taskArgs=@($taskEngine);if($taskMode -in @('tamper','disabled')){$taskArgs+='--'+$taskMode}
 $taskResult=& (Join-Path $taskFixture 'Turing Complete.exe') @taskArgs
 if($LASTEXITCODE){throw "Native $taskMode test failed: $taskResult"}
 $taskResult
 if($taskMode -eq 'conflict') {
  $taskText=$taskResult -join "`n"
  if($taskText -notmatch 'raw plain-hook ok=1') {throw 'The first raw hook was not accepted'}
  if($taskText -notmatch 'raw plain-hook ok=0') {throw 'The duplicate raw hook was not refused'}
  if($taskText -notmatch 'Hook rejected') {throw 'Hook conflict was not detected'}
 }
}
