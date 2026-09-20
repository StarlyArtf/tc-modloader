# Development-only exact-callsite probe; no real-mouse or public-API claim.
param([int]$Seconds=35)
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskCompiler=if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskPackage=Join-Path $taskRepo 'build\board-slot-probe-package'
$taskMod=Join-Path $taskRepo 'dist\dev.board-slot-probe.mod'
$taskSandbox=Join-Path $taskRepo 'build\board-slot-probe-sandbox'
$taskRoot=Join-Path $taskSandbox 'game'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native') | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared (Join-Path $PSScriptRoot 'ui-board-slot-probe.cpp') -o (Join-Path $taskPackage 'native\probe.dll')
if($LASTEXITCODE){throw 'Board slot probe compilation failed'}
'{"format":2,"id":"dev.board-slot-probe","name":"Board slot probe","version":"0.1.0","native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content (Join-Path $taskPackage 'mod.json') -Encoding ascii
if(Test-Path -LiteralPath $taskMod){Remove-Item -LiteralPath $taskMod}
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output $taskMod
& (Join-Path $PSScriptRoot 'make-ui-sandbox.ps1') -Sandbox $taskSandbox -Mods dev.board-slot-probe
$taskLog=Join-Path $taskRoot 'tc-modloader-data\loader.log'
Remove-Item -LiteralPath $taskLog -ErrorAction SilentlyContinue
$taskPreviousProfile=$env:USERPROFILE;$taskPreviousAppData=$env:APPDATA;$taskProcess=$null
try {
    $env:USERPROFILE=Join-Path $taskSandbox 'home'
    $env:APPDATA=Join-Path $env:USERPROFILE 'AppData\Roaming'
    $taskProcess=Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline=(Get-Date).AddSeconds($Seconds)
    do {
        Start-Sleep -Milliseconds 500
        $taskText=if(Test-Path -LiteralPath $taskLog){Get-Content -LiteralPath $taskLog -Raw}else{''}
        if($taskText -match 'BOARD SLOT FAIL|BOARD SLOT PASS frames=180|Native failed|Plugin callback threw' -or $taskProcess.HasExited){break}
    }while((Get-Date)-lt $taskDeadline)
}finally{
    $env:USERPROFILE=$taskPreviousProfile;$env:APPDATA=$taskPreviousAppData
    if($taskProcess -and !$taskProcess.HasExited){[void]$taskProcess.CloseMainWindow();if(!$taskProcess.WaitForExit(3000)){Stop-Process -Id $taskProcess.Id}}
}
$taskText=Get-Content -LiteralPath $taskLog -Raw
foreach($taskExpected in @('BOARD SLOT entered sandbox','BOARD SLOT PASS synthetic active','BOARD SLOT PASS inactive','BOARD SLOT PASS frames=180')){
    if(!$taskText.Contains($taskExpected)){Get-Content -LiteralPath $taskLog -Tail 30;throw "Missing: $taskExpected"}
}
if($taskText -match 'BOARD SLOT FAIL|Native failed|Plugin callback threw'){throw 'Board slot probe reported failure'}
Get-Content -LiteralPath $taskLog | Select-String 'BOARD SLOT'
'PASS native board injection timing (synthetic ActiveID), child/canvas geometry and 180 frames; real clicks and scene exit are NOT verified'
