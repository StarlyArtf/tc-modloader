$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskGame=Split-Path $taskRepo
$taskEngine=Join-Path $taskGame 'tc_game_engine.dll'
if (!(Test-Path $taskEngine)) {$taskEngine=Join-Path $taskGame 'game_engine.dll'}
foreach($taskMode in @('normal','tamper','conflict','disabled')) {
 $taskFixture=Join-Path $taskRepo ('build\native-test-'+$taskMode+'-'+[guid]::NewGuid().ToString('N'))
 New-Item -ItemType Directory -Force (Join-Path $taskFixture 'mods') | Out-Null
 Copy-Item (Join-Path $taskRepo 'build\native-host.exe') (Join-Path $taskFixture 'Turing Complete.exe')
 Copy-Item (Join-Path $taskRepo 'dist\example.cycle-guard.mod') (Join-Path $taskFixture 'mods\guard.mod')
 if($taskMode -eq 'conflict'){
  $taskConflict=Join-Path $taskFixture 'conflict-source'
  New-Item -ItemType Directory -Force (Join-Path $taskConflict 'native') | Out-Null
  $taskManifestPath=Join-Path $taskRepo 'examples\cycle-guard\mod.json'
  $taskManifestText=[System.IO.File]::ReadAllText($taskManifestPath,[System.Text.Encoding]::UTF8)
  $taskManifest=$taskManifestText | ConvertFrom-Json
  $taskManifest.id='z.conflict'
  $taskManifest | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $taskConflict 'mod.json') -Encoding utf8
  Copy-Item (Join-Path $taskRepo 'examples\cycle-guard\native\cycle-guard.dll') (Join-Path $taskConflict 'native')
  & (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskConflict -Output (Join-Path $taskFixture 'mods\conflict.mod') | Out-Null
 }
 $taskArgs=@($taskEngine);if($taskMode -in @('tamper','disabled')){$taskArgs+='--'+$taskMode}
 $taskResult=& (Join-Path $taskFixture 'Turing Complete.exe') @taskArgs
 if($LASTEXITCODE){throw "Native $taskMode test failed: $taskResult"}
 $taskResult
 if($taskMode -eq 'conflict' -and ($taskResult -join "`n") -notmatch 'Hook rejected') {throw 'Hook conflict was not detected'}
}
