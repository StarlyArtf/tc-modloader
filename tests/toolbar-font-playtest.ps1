# Development probe for the toolbar text question.  Builds tests/toolbar-font-probe.cpp into
# dev.toolbar-font-probe, runs it beside the enter-board probe in an isolated copy of the game
# and takes a screenshot from inside the process (the sandbox window is parked off the desktop,
# so nothing outside the process can see it).
param(
  [int]$Seconds = 40,
  [int]$ShotDelay = 14000,
  [string]$TraceText = ''
)
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskCompiler=if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskPackage=Join-Path $taskRepo 'build\toolbar-font-package'
$taskMod=Join-Path $taskRepo 'dist\dev.toolbar-font-probe.mod'
$taskSandbox=Join-Path $taskRepo 'build\toolbar-font-sandbox'
$taskRoot=Join-Path $taskSandbox 'game'
$taskOut=Join-Path $taskRepo 'build\toolbar-font-out'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native'),$taskOut | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared (Join-Path $PSScriptRoot 'toolbar-font-probe.cpp') -o (Join-Path $taskPackage 'native\probe.dll')
if($LASTEXITCODE){throw 'Toolbar font probe compilation failed'}
'{"format":2,"id":"dev.toolbar-font-probe","name":"Toolbar font probe","version":"0.1.0","native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content (Join-Path $taskPackage 'mod.json') -Encoding ascii
if(Test-Path -LiteralPath $taskMod){Remove-Item -LiteralPath $taskMod}
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output $taskMod
& (Join-Path $PSScriptRoot 'make-ui-sandbox.ps1') -Sandbox $taskSandbox -Mods dev.enter-board,dev.toolbar-font-probe
$taskLog=Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskShot=Join-Path $taskOut 'shot.bmp'
Remove-Item -LiteralPath $taskLog,$taskShot -ErrorAction SilentlyContinue
$taskPreviousProfile=$env:USERPROFILE;$taskPreviousAppData=$env:APPDATA;$taskPreviousShot=$env:TC_MODLOADER_SHOT;$taskPreviousDelay=$env:TC_MODLOADER_SHOT_DELAY;$taskPreviousTrace=$env:TC_MODLOADER_TRACE_TEXT;$taskProcess=$null
try {
    $env:USERPROFILE=Join-Path $taskSandbox 'home'
    $env:APPDATA=Join-Path $env:USERPROFILE 'AppData\Roaming'
    $env:TC_MODLOADER_SHOT=$taskShot
    $env:TC_MODLOADER_SHOT_DELAY="$ShotDelay"
    $env:TC_MODLOADER_TRACE_TEXT=$TraceText
    $taskProcess=Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline=(Get-Date).AddSeconds($Seconds)
    do {
        Start-Sleep -Milliseconds 500
        $taskText=if(Test-Path -LiteralPath $taskLog){Get-Content -LiteralPath $taskLog -Raw}else{''}
        if($taskText -match 'Captured frame screenshot' -or $taskProcess.HasExited){break}
    }while((Get-Date)-lt $taskDeadline)
}finally{
    $env:USERPROFILE=$taskPreviousProfile;$env:APPDATA=$taskPreviousAppData
    $env:TC_MODLOADER_SHOT=$taskPreviousShot;$env:TC_MODLOADER_SHOT_DELAY=$taskPreviousDelay
    $env:TC_MODLOADER_TRACE_TEXT=$taskPreviousTrace
    if($taskProcess -and !$taskProcess.HasExited){[void]$taskProcess.CloseMainWindow();if(!$taskProcess.WaitForExit(3000)){Stop-Process -Id $taskProcess.Id}}
}
if(!(Test-Path -LiteralPath $taskLog)){throw 'No loader log: the game did not start'}
Get-Content -LiteralPath $taskLog | Select-String 'TOOLBARFONT|Captured frame screenshot|ENTER-BOARD' | ForEach-Object { $_.Line }
if(Test-Path -LiteralPath $taskShot){
    Add-Type -AssemblyName System.Drawing
    $taskImage=[System.Drawing.Image]::FromFile($taskShot)
    $taskPng=Join-Path $taskOut 'shot.png'
    $taskImage.Save($taskPng,[System.Drawing.Imaging.ImageFormat]::Png)
    $taskImage.Dispose()
    "Screenshot: $taskPng"
}else{
    'No screenshot was taken'
}
