param([int]$Seconds=30)
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskGame=Split-Path $taskRepo
$taskCompiler=if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskSandbox=Join-Path $taskRepo 'build\component-geometry-playtest'
$taskRoot=Join-Path $taskSandbox 'game'
$taskHome=Join-Path $taskSandbox 'home'
$taskPackage=Join-Path $taskSandbox 'package'
if(Test-Path -LiteralPath $taskSandbox){
  $taskResolved=(Resolve-Path -LiteralPath $taskSandbox).Path
  $taskBuild=(Resolve-Path -LiteralPath (Join-Path $taskRepo 'build')).Path
  if(-not $taskResolved.StartsWith($taskBuild)){throw "Refusing to remove outside build/: $taskResolved"}
  Remove-Item -LiteralPath $taskSandbox -Recurse -Force
}
New-Item -ItemType Directory -Force $taskRoot,$taskHome,(Join-Path $taskRoot 'mods'),(Join-Path $taskPackage 'native')|Out-Null
foreach($taskDir in @('asset','campaign','translations')){Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')){
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk (Join-Path $PSScriptRoot 'component-geometry-probe.cpp') -o (Join-Path $taskPackage 'native\probe.dll')
if($LASTEXITCODE){throw 'Component geometry probe build failed'}
'{"format":2,"id":"dev.component-geometry-probe","name":"Component geometry probe","version":"0.1.0","capabilities":["log","symbol","component","services"],"native":{"api":1,"entry":"native/probe.dll"}}'|Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding ascii
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRoot 'mods\probe.mod')|Out-Null
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\example.byte-adder.mod') -Destination (Join-Path $taskRoot 'mods')
$taskDriverData=Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.byte-adder'
New-Item -ItemType Directory -Force $taskDriverData|Out-Null
Set-Content -LiteralPath (Join-Path $taskDriverData 'autotest.txt') -Value 'byte_adder' -Encoding ascii
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply example.byte-adder dev.component-geometry-probe
if($LASTEXITCODE){throw 'Component geometry probe apply failed'}
$taskData=Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.component-geometry-probe'
$taskResult=Join-Path $taskData 'result.txt'
$taskPreviousProfile=$env:USERPROFILE;$taskPreviousAppData=$env:APPDATA;$taskProcess=$null
try{
  $env:USERPROFILE=$taskHome;$env:APPDATA=Join-Path $taskHome 'AppData\Roaming'
  $taskProcess=Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline=[DateTime]::UtcNow.AddSeconds($Seconds)
  while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline){Start-Sleep -Milliseconds 250}
  if(!(Test-Path -LiteralPath $taskResult)){throw "Geometry probe did not finish; inspect $taskRoot\tc-modloader-data\loader.log"}
  $taskText=[IO.File]::ReadAllText($taskResult);$taskText
  if(!$taskText.StartsWith('PASS ')){throw "Geometry probe failed: $taskText"}
}finally{
  $env:USERPROFILE=$taskPreviousProfile;$env:APPDATA=$taskPreviousAppData
  if($taskProcess -and !$taskProcess.HasExited){[void]$taskProcess.CloseMainWindow();if(!$taskProcess.WaitForExit(3000)){Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue}}
  "Evidence: $taskSandbox"
}
