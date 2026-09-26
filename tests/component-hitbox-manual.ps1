# Manual hit-area measurement.
#
# The synthetic probe (component-hitbox-playtest.ps1) drives the real cursor and
# has proved too fragile: the same offset answers differently depending on
# hover, timing and whether the pointer is handed back after a sample.  This
# case injects nothing at all.  It places the three probe types on a sandbox
# board, leaves the game in the foreground, and records every human mouse press
# with the board coordinates the game itself computed, plus what the game has
# selected once the click has settled.  Press Enter here when you are done
# clicking.
#
# What makes the log decidable is the *pair* of lines per click:
#   hitbox: manual press   board=(x,y) selected=<what was selected before>
#   hitbox: manual settled board=(x,y) selected=<what is selected after>
# A click that selects a component is the one whose settled line names it.  The
# selection is sticky, so a press-time reading alone says nothing - the middle
# component stayed "selected" for presses far outside it in the 2026-09-22 run.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests/component-hitbox-manual.ps1
param(
  [int]$Seconds = 900,
  [int]$StopAfterPresses = 40
)
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskGame=Split-Path $taskRepo
$taskCompiler=if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskSandbox=Join-Path $taskRepo 'build\hitbox-manual-sandbox'
$taskRoot=Join-Path $taskSandbox 'game'
$taskOut=Join-Path $taskRepo 'build\hitbox-manual-out'
$taskPackage=Join-Path $taskRepo 'build\hitbox-manual-package'
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
Remove-Item -LiteralPath $taskLog -ErrorAction SilentlyContinue
$taskPreviousProfile=$env:USERPROFILE;$taskPreviousAppData=$env:APPDATA;$taskPreviousScan=$env:TC_HITBOX_SCAN
$taskProcess=$null
try {
    $env:USERPROFILE=Join-Path $taskSandbox 'home'
    $env:APPDATA=Join-Path $env:USERPROFILE 'AppData\Roaming'
    $env:TC_HITBOX_SCAN='manual'
    Write-Host ''
    Write-Host 'A sandbox copy of the game is starting in its own window.  Nothing is injected;'
    Write-Host 'the probe only listens.  When the board is up it places three custom components'
    Write-Host '(left / middle / right) and then records every press you make.'
    Write-Host ''
    Write-Host 'Plain clicks are what this measures (press and release, no dragging).  Do this:'
    Write-Host '  1. click empty board a few squares away       (resets the selection)'
    Write-Host '  2. click the LEFT component dead centre'
    Write-Host '  3. click empty board again, then the LEFT one near each corner of its body,'
    Write-Host '     then a point just outside it, always with an empty click in between'
    Write-Host '  4. repeat for the MIDDLE component (its declared footprint is 12x6, so it is'
    Write-Host '     much wider: click the centre, the four inner corners, then well outside)'
    Write-Host '  5. finish with a couple of clicks on the RIGHT component, which is rotated 90'
    Write-Host ''
    Write-Host 'An empty-board click between probes matters: the game keeps the previous'
    Write-Host 'selection until something clears it, and a press-time reading alone cannot'
    Write-Host 'tell "this click hit" from "the last click is still selected".'
    Write-Host ''
    Write-Host "This script stops by itself after $StopAfterPresses recorded clicks (or after $Seconds seconds)."
    $taskProcess=Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -PassThru
    $taskDeadline=(Get-Date).AddSeconds($Seconds)
    $taskReady=$false
    $taskSeen=0
    while((Get-Date) -lt $taskDeadline -and !$taskProcess.HasExited){
        if(Test-Path -LiteralPath $taskLog){
            $taskText=Get-Content -LiteralPath $taskLog -Raw
            if(!$taskReady -and $taskText -match 'hitbox: manual ready'){
                $taskReady=$true
                Write-Host 'READY: the three components are placed.  Start clicking now.'
            }
            $taskCount=($taskText | Select-String 'hitbox: manual press' -AllMatches).Matches.Count
            if($taskCount -ne $taskSeen){
                $taskSeen=$taskCount
                Write-Host ("recorded {0}/{1} clicks" -f $taskSeen,$StopAfterPresses)
            }
            if($taskSeen -ge $StopAfterPresses){break}
        }
        Start-Sleep -Milliseconds 250
    }
}finally{
    $env:USERPROFILE=$taskPreviousProfile;$env:APPDATA=$taskPreviousAppData;$env:TC_HITBOX_SCAN=$taskPreviousScan
    if($taskProcess -and !$taskProcess.HasExited){
        [void]$taskProcess.CloseMainWindow()
        if(!$taskProcess.WaitForExit(4000)){Stop-Process -Id $taskProcess.Id}
    }
}
if(!(Test-Path -LiteralPath $taskLog)){throw 'No loader log: the game did not start'}
$taskText=Get-Content -LiteralPath $taskLog -Raw
$taskLines=Get-Content -LiteralPath $taskLog | Select-String 'hitbox: (manual|found|place|box)' | ForEach-Object { $_.Line }
$taskLines | ForEach-Object { $_ }
$taskLines | Set-Content -LiteralPath (Join-Path $taskOut 'clicks.txt') -Encoding utf8
$taskPresses=@(Get-Content -LiteralPath $taskLog | Select-String 'hitbox: manual press')
if($taskPresses.Count -eq 0){throw 'No human presses were recorded; run the script again and click inside the game window'}
$taskSettled=@(Get-Content -LiteralPath $taskLog | Select-String 'hitbox: manual settled')
Write-Host ''
Write-Host "settled readings ($($taskSettled.Count)) - these are the ones that answer 'did this click select it':"
$taskSettled | ForEach-Object { $_.Line }
"Recorded $($taskPresses.Count) presses"
"Evidence: $(Join-Path $taskOut 'clicks.txt')"
"Sandbox: $taskSandbox"
"PASS recorded $($taskPresses.Count) human presses with board coordinates"
