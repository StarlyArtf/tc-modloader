param([Parameter(Mandatory=$true)][string]$Circuit)
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskGame=Split-Path $taskRepo
$taskCircuit=(Resolve-Path -LiteralPath $Circuit).Path
$taskHash=(Get-FileHash -LiteralPath $taskCircuit).Hash
$taskTest=Join-Path $taskRepo ('build\component-playtest-'+[guid]::NewGuid().ToString('N'))
$taskRoot=Join-Path $taskTest 'game'
$taskProfile=Join-Path $taskTest 'home'
$taskData=Join-Path $taskRoot 'tc-modloader-data\plugin-data\test.component-probe'
$taskPackage=Join-Path $taskTest 'package'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods'),(Join-Path $taskData 'fixture'),(Join-Path $taskPackage 'native') | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
 Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
 Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath $taskCircuit -Destination (Join-Path $taskData 'fixture\circuit.data')
Copy-Item -LiteralPath (Join-Path $taskRepo 'build\component-probe.dll') -Destination (Join-Path $taskPackage 'native\probe.dll')
'{"format":2,"id":"test.component-probe","name":"Development component probe","version":"0.0.1","native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding utf8
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRoot 'mods\probe.mod') | Out-Null
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.component-probe
if($LASTEXITCODE){throw 'Probe package apply failed'}
$taskPreviousProfile=$env:USERPROFILE
$taskPreviousAppData=$env:APPDATA
$taskProcess=$null
try {
 $env:USERPROFILE=$taskProfile
 $env:APPDATA=Join-Path $taskProfile 'AppData\Roaming'
 $taskProcess=Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
 $taskDeadline=[DateTime]::UtcNow.AddSeconds(40)
 $taskResult=Join-Path $taskData 'result.txt'
 while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline){Start-Sleep -Milliseconds 250}
 if(!(Test-Path -LiteralPath $taskResult)){throw "Probe did not finish; inspect $taskRoot\tc-modloader-data\loader.log"}
 $taskText=[IO.File]::ReadAllText($taskResult)
 $taskText
 if(!$taskText.StartsWith('PASS ')){throw "Probe failed: $taskText"}
} finally {
 $env:USERPROFILE=$taskPreviousProfile
 $env:APPDATA=$taskPreviousAppData
 if($taskProcess -and !$taskProcess.HasExited){
  [void]$taskProcess.CloseMainWindow()
  if(!$taskProcess.WaitForExit(3000)){Stop-Process -Id $taskProcess.Id}
 }
 "Evidence: $taskTest"
 if((Get-FileHash -LiteralPath $taskCircuit).Hash -ne $taskHash){throw 'Source fixture changed'}
}
