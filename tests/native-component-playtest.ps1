param([switch]$Single)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskCompiler = if($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
Push-Location $taskRepo
try {
    $taskPackage = Join-Path $taskRepo 'build\declarative-stress-package'
    New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native') | Out-Null
    $taskFlags = @()
    if($Single) { $taskFlags += '-DTC_DECLARATIVE_SINGLE' }
    & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared @taskFlags tests/native-component-probe.cpp -o (Join-Path $taskPackage 'native\byte-adder.dll')
    if($LASTEXITCODE) { throw 'Probe compilation failed' }
    Copy-Item examples/byte-adder/mod.json $taskPackage -Force
    $taskMod = Join-Path $taskRepo ('build/declarative-stress-' + [guid]::NewGuid().ToString('N') + '.mod')
    & ./tools/Pack-Mod.ps1 -Source $taskPackage -Output $taskMod
    & "$taskCompiler\g++.exe" -std=c++17 -O2 -static tests/native-component-board.cpp -o build/native-component-board.exe
    if($LASTEXITCODE) { throw 'Board compilation failed' }
    & ./build/native-component-board.exe
    if($LASTEXITCODE) { throw 'Board generation failed' }
    $taskBoard = if($Single) { 'build/nl_double8board.data' } else { 'build/declarative-8x8-board.data' }
    & ./tests/byte-adder-smoke.ps1 -PackagePath $taskMod -SchematicPath (Join-Path $taskRepo $taskBoard) -Stress -Single:$Single
} finally { Pop-Location }
