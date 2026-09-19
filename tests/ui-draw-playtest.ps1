# Tests real engine geometry and continued frames in an isolated game/profile.
param([int]$Seconds = 25)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskPackage = Join-Path $taskRepo 'build\draw-test-package'
$taskMod = Join-Path $taskRepo 'dist\dev.draw-test.mod'
$taskSandbox = Join-Path $taskRepo 'build\draw-test-sandbox'
$taskRoot = Join-Path $taskSandbox 'game'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native') | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared (Join-Path $PSScriptRoot 'ui-draw-engine.cpp') -o (Join-Path $taskPackage 'native\probe.dll')
if ($LASTEXITCODE) { throw 'Drawing probe compilation failed' }
'{"format":2,"id":"dev.draw-test","name":"Drawing integration test","version":"0.1.0","native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content (Join-Path $taskPackage 'mod.json') -Encoding ascii
if (Test-Path -LiteralPath $taskMod) { Remove-Item -LiteralPath $taskMod }
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output $taskMod
& (Join-Path $PSScriptRoot 'make-ui-sandbox.ps1') -Sandbox $taskSandbox -Mods dev.draw-test
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
Remove-Item -LiteralPath $taskLog -ErrorAction SilentlyContinue
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskProcess = $null
try {
    $env:USERPROFILE = Join-Path $taskSandbox 'home'
    $env:APPDATA = Join-Path $env:USERPROFILE 'AppData\Roaming'
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline = (Get-Date).AddSeconds($Seconds)
    do {
        Start-Sleep -Milliseconds 500
        $taskText = if (Test-Path -LiteralPath $taskLog) { Get-Content -LiteralPath $taskLog -Raw } else { '' }
        if ($taskText -match 'DRAW FAIL|DRAW PASS frames=180' -or $taskProcess.HasExited) { break }
    } while ((Get-Date) -lt $taskDeadline)
} finally {
    $env:USERPROFILE = $taskPreviousProfile
    $env:APPDATA = $taskPreviousAppData
    if ($taskProcess -and !$taskProcess.HasExited) {
        [void]$taskProcess.CloseMainWindow()
        if (!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
    }
}
$taskText = Get-Content -LiteralPath $taskLog -Raw
if ($taskText -match 'DRAW FAIL|Native failed|Plugin callback threw' -or $taskText -notmatch 'DRAW PASS frames=180') {
    Get-Content -LiteralPath $taskLog -Tail 25
    throw 'Drawing engine regression failed'
}
Get-Content -LiteralPath $taskLog | Select-String 'DRAW PASS'
'PASS real-engine drawing, vertex positions/colors, move/resize, child scrolling, nested clipping; 180 frames'
