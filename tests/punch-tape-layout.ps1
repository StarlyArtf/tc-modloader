$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskCompiler = if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskBuild = Join-Path $taskRepo 'build'
New-Item -ItemType Directory -Force $taskBuild | Out-Null
$taskExe = Join-Path $taskBuild 'punch-tape-layout-test.exe'
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Werror -static `
  (Join-Path $PSScriptRoot 'punch-tape-layout.cpp') -o $taskExe
if($LASTEXITCODE){ throw 'Punch tape layout test did not compile' }
& $taskExe
if($LASTEXITCODE){ throw 'Punch tape layout test failed' }
