$ErrorActionPreference='Stop'
Push-Location $PSScriptRoot
try {
 # GNU ld embeds the current time in PE files unless SOURCE_DATE_EPOCH is set, and
 # the release pipeline refuses a tree its own build changes.  Same default as
 # build.ps1, so a rebuild of unchanged sources produces the same .mod bytes and
 # "the sandbox tested these bytes" stays checkable by hash.
 if (!$env:SOURCE_DATE_EPOCH) { $env:SOURCE_DATE_EPOCH = '315532800' }
 $taskCompiler='C:\msys64\ucrt64\bin'
 if($env:TC_MINGW_BIN){$taskCompiler=$env:TC_MINGW_BIN}
 $taskStage=Join-Path $PSScriptRoot 'build\text-box-package'
 if(Test-Path -LiteralPath $taskStage){Remove-Item -LiteralPath $taskStage -Recurse -Force}
 New-Item -ItemType Directory -Force examples\text-box\native,dist,(Join-Path $taskStage 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Wno-misleading-indentation -Wno-cast-function-type -static -shared -Isdk examples\text-box\plugin.cpp -lopengl32 -o examples\text-box\native\text-box.dll
 if($LASTEXITCODE){throw 'Text box compilation failed'}
 Copy-Item examples\text-box\mod.json $taskStage -Force
 Copy-Item examples\text-box\native\text-box.dll (Join-Path $taskStage 'native') -Force
 $taskOutput=Join-Path $PSScriptRoot 'dist\local.text-box.mod'
 if(Test-Path -LiteralPath $taskOutput){Remove-Item -LiteralPath $taskOutput}
 & .\tools\Pack-Mod.ps1 -Source $taskStage -Output $taskOutput
 if($LASTEXITCODE){throw 'Text box packaging failed'}
 Write-Host "Built $taskOutput"
}finally{Pop-Location}
