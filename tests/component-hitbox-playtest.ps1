# Runs the component hit-area probe in an isolated copy of the game. It compares
# the stock shape with a 12x6 tc.component.geometry footprint at the same two
# pointer offsets. The expected equal result is a regression test for the
# measured fact that footprint and pointer hit testing are separate paths.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests/component-hitbox-playtest.ps1
param(
  [int]$Seconds = 120,
  [int]$ShotDelay = 30000,
  [switch]$KeepRunning
)
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskGame=Split-Path $taskRepo
$taskCompiler=if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskSandbox=Join-Path $taskRepo 'build\hitbox-sandbox'
$taskRoot=Join-Path $taskSandbox 'game'
$taskOut=Join-Path $taskRepo 'build\hitbox-out'

$taskPackage=Join-Path $taskRepo 'build\hitbox-package'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native'),$taskOut | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk (Join-Path $PSScriptRoot 'component-hitbox-probe.cpp') -o (Join-Path $taskPackage 'native\hitbox.dll')
if($LASTEXITCODE){throw 'Hitbox probe build failed'}
'{"format":2,"id":"dev.hitbox-probe","name":"Component hit-area probe","version":"0.1.0","capabilities":["log","component","services","symbol","symbol_alias","game_handles"],"native":{"api":1,"entry":"native/hitbox.dll"}}' | Set-Content (Join-Path $taskPackage 'mod.json') -Encoding ascii
if(Test-Path -LiteralPath (Join-Path $taskRepo 'dist\dev.hitbox-probe.mod')){Remove-Item -LiteralPath (Join-Path $taskRepo 'dist\dev.hitbox-probe.mod')}
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRepo 'dist\dev.hitbox-probe.mod')

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
foreach ($taskMod in @('dev.hitbox-probe','dev.enter-board')) {
    Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\'+$taskMod+'.mod')) -Destination (Join-Path $taskRoot 'mods') -Force
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.hitbox-probe dev.enter-board
if ($LASTEXITCODE) { throw 'Sandbox mod apply failed' }

$taskLog=Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskShot=Join-Path $taskOut 'board.bmp'
Remove-Item -LiteralPath $taskLog,$taskShot -ErrorAction SilentlyContinue
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
        Start-Sleep -Milliseconds 500
        $taskText=if(Test-Path -LiteralPath $taskLog){Get-Content -LiteralPath $taskLog -Raw}else{''}
        if($taskText -match 'hitbox: scan finished' -or
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
Get-Content -LiteralPath $taskLog | Select-String 'hitbox:|ENTER-BOARD' | ForEach-Object { $_.Line }
$taskText=Get-Content -LiteralPath $taskLog -Raw
foreach($taskExpected in @(
    'hitbox: hit-default offset=\(1,0\).*selected=0',
    'hitbox: hit-default offset=\(2,0\).*selected=1',
    'hitbox: hit-expanded offset=\(1,0\).*selected=0',
    'hitbox: hit-expanded offset=\(2,0\).*selected=1',
    'hitbox: scan finished')){
    if($taskText -notmatch $taskExpected){throw "Missing hit-area evidence: $taskExpected"}
}
if(Test-Path -LiteralPath $taskShot){
    Add-Type -AssemblyName System.Drawing
    $taskImage=[System.Drawing.Image]::FromFile($taskShot)
    $taskPng=Join-Path $taskOut 'board.png'
    $taskImage.Save($taskPng,[System.Drawing.Imaging.ImageFormat]::Png)
    $taskImage.Dispose()
    "Screenshot: $taskPng"
}
"Sandbox: $taskSandbox"
"PASS component footprint is independent from pointer hit testing"
