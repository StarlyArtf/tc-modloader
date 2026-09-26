# Runs the appearance probe in an isolated copy of the game and prints the
# evidence it produced.  The probe works at load time, so no level is needed.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests/component-appearance-playtest.ps1
param(
  [int]$Seconds = 60,
  [int]$ShotDelay = 26000,
  [switch]$KeepRunning
)
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskGame=Split-Path $taskRepo
$taskCompiler=if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskSandbox=Join-Path $taskRepo 'build\appearance-sandbox'
$taskRoot=Join-Path $taskSandbox 'game'

$taskPackage=Join-Path $taskRepo 'build\appearance-package'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native') | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk (Join-Path $PSScriptRoot 'component-appearance-probe.cpp') -o (Join-Path $taskPackage 'native\appearance.dll')
if($LASTEXITCODE){throw 'Appearance probe build failed'}
'{"format":2,"id":"dev.appearance-probe","name":"Component appearance probe","version":"0.1.0","capabilities":["log"],"native":{"api":1,"entry":"native/appearance.dll"}}' | Set-Content (Join-Path $taskPackage 'mod.json') -Encoding ascii
if(Test-Path -LiteralPath (Join-Path $taskRepo 'dist\dev.appearance-probe.mod')){Remove-Item -LiteralPath (Join-Path $taskRepo 'dist\dev.appearance-probe.mod')}
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRepo 'dist\dev.appearance-probe.mod')

if (Test-Path -LiteralPath $taskSandbox) {
    $resolved=(Resolve-Path -LiteralPath $taskSandbox).Path
    if (-not $resolved.StartsWith((Resolve-Path -LiteralPath (Join-Path $taskRepo 'build')).Path)) {
        throw "Refusing to remove a sandbox outside build/: $taskSandbox"
    }
    Remove-Item -LiteralPath $taskSandbox -Recurse -Force
}
New-Item -ItemType Directory -Force $taskRoot,(Join-Path $taskSandbox 'home'),(Join-Path $taskRoot 'mods') | Out-Null
foreach ($taskDir in @('asset','campaign','translations')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot -Force
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
foreach ($taskMod in @('dev.appearance-probe','dev.enter-board')) {
    Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\'+$taskMod+'.mod')) -Destination (Join-Path $taskRoot 'mods') -Force
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.appearance-probe dev.enter-board
if ($LASTEXITCODE) { throw 'Sandbox mod apply failed' }

$taskLog=Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskShot=Join-Path $taskRepo 'build\appearance-out\board.bmp'
New-Item -ItemType Directory -Force (Split-Path $taskShot) | Out-Null
Remove-Item -LiteralPath $taskShot -ErrorAction SilentlyContinue
$taskPreviousProfile=$env:USERPROFILE;$taskPreviousAppData=$env:APPDATA
$taskPreviousShot=$env:TC_MODLOADER_SHOT;$taskPreviousDelay=$env:TC_MODLOADER_SHOT_DELAY
$taskProcess=$null
try {
    $env:USERPROFILE=Join-Path $taskSandbox 'home'
    $env:APPDATA=Join-Path $env:USERPROFILE 'AppData\Roaming'
    $env:TC_MODLOADER_SHOT=$taskShot
    $env:TC_MODLOADER_SHOT_DELAY="$ShotDelay"
    $taskProcess=Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline=(Get-Date).AddSeconds($Seconds)
    do {
        Start-Sleep -Milliseconds 400
        $taskText=if(Test-Path -LiteralPath $taskLog){Get-Content -LiteralPath $taskLog -Raw}else{''}
        if($taskText -match 'Captured frame screenshot' -or
           $taskText -match 'Plugin callback threw|Native failed' -or
           $taskProcess.HasExited){break}
    }while((Get-Date)-lt $taskDeadline)
}finally{
    $env:USERPROFILE=$taskPreviousProfile;$env:APPDATA=$taskPreviousAppData
    $env:TC_MODLOADER_SHOT=$taskPreviousShot;$env:TC_MODLOADER_SHOT_DELAY=$taskPreviousDelay
    if($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning){
        [void]$taskProcess.CloseMainWindow()
        if(!$taskProcess.WaitForExit(4000)){Stop-Process -Id $taskProcess.Id}
    }
}
if(!(Test-Path -LiteralPath $taskLog)){throw 'No loader log: the game did not start'}
Get-Content -LiteralPath $taskLog | Select-String 'appearance:' | ForEach-Object { $_.Line }
if(Test-Path -LiteralPath $taskShot){
    Add-Type -AssemblyName System.Drawing
    $taskImage=[System.Drawing.Image]::FromFile($taskShot)
    $taskPng=Join-Path (Split-Path $taskShot) 'board.png'
    $taskImage.Save($taskPng,[System.Drawing.Imaging.ImageFormat]::Png)
    $taskImage.Dispose()
    "Screenshot: $taskPng"
}
"Sandbox: $taskSandbox"

