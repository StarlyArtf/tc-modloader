$ErrorActionPreference='Stop'
Push-Location $PSScriptRoot
try {
 $taskCompiler='C:\msys64\ucrt64\bin'
 if($env:TC_MINGW_BIN){$taskCompiler=$env:TC_MINGW_BIN}
 $taskStage=Join-Path $PSScriptRoot 'build\wire-palette-package'
 New-Item -ItemType Directory -Force examples\wire-palette\native,dist,(Join-Path $taskStage 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared examples\wire-palette\plugin.cpp -lopengl32 -o examples\wire-palette\native\wire-palette.dll
 if($LASTEXITCODE){throw 'Wire Palette compilation failed'}
 Copy-Item examples\wire-palette\mod.json $taskStage -Force
 Copy-Item examples\wire-palette\native\wire-palette.dll (Join-Path $taskStage 'native') -Force
 $taskOutput=Join-Path $PSScriptRoot 'dist\local.wire-palette.mod'
 if(Test-Path -LiteralPath $taskOutput){Remove-Item -LiteralPath $taskOutput}
 & .\tools\Pack-Mod.ps1 -Source $taskStage -Output $taskOutput
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static tests\wire-palette.cpp -o build\wire-palette-test.exe
 if($LASTEXITCODE){throw 'Palette test compilation failed'}
 & .\build\wire-palette-test.exe
 if($LASTEXITCODE){throw 'Palette tests failed'}
}finally{Pop-Location}

