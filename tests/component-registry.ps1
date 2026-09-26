$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskCompiler = if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskBuild = Join-Path $taskRepo 'build'
New-Item -ItemType Directory -Force $taskBuild | Out-Null
$taskExe = Join-Path $taskBuild 'component-registry-test.exe'
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Werror -static `
  (Join-Path $PSScriptRoot 'component-registry.cpp') -o $taskExe
if($LASTEXITCODE){ throw 'Component registry test did not compile' }
& $taskExe
if($LASTEXITCODE){ throw 'Component registry test failed' }
