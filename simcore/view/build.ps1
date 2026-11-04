# Builds simcore-view, the standalone window that draws what simcore thinks a
# board is.  Same shape as simcore/tests/simcore.ps1: the repository has no
# CMake, every offline build here is a PowerShell script next to the code, and
# this program must stay buildable with nothing but MinGW-w64.
#
# gdi32 (drawing), user32 (the window) and comdlg32 (the board picker) are
# Windows system libraries, not third-party dependencies, so simcore keeps its
# "compiles and runs with no game attached" property.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path (Split-Path $PSScriptRoot)
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskBuild = Join-Path $taskRepo 'build'
New-Item -ItemType Directory -Force $taskBuild | Out-Null
$taskExe = Join-Path $taskBuild 'simcore-view.exe'
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Werror -static `
    (Join-Path $PSScriptRoot 'view_main.cpp') -o $taskExe -lgdi32 -luser32 -lcomdlg32
if ($LASTEXITCODE) { throw 'simcore-view did not compile' }
"Saved: $taskExe"
