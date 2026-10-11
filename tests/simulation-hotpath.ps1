$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskCompilerRoot = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
Push-Location $taskRepo
try {
    foreach ($taskCase in @('simulation-hotpath', 'scope-capture', 'simulation-jit')) {
        $taskBinary = Join-Path $taskRepo "build\$taskCase-test.exe"
        $taskLinkObjects = @()
        if ($taskCase -eq 'simulation-jit') {
            $taskDecoder = Join-Path $taskRepo 'build\simulation-jit-hde64.o'
            & (Join-Path $taskCompilerRoot 'gcc.exe') -O2 -c 'vendor\minhook\src\hde\hde64.c' -o $taskDecoder
            if ($LASTEXITCODE) { throw 'JIT decoder compilation failed' }
            $taskLinkObjects = @($taskDecoder)
        }
        & (Join-Path $taskCompilerRoot 'g++.exe') -std=c++17 -O2 -static "tests\$taskCase.cpp" @taskLinkObjects -o $taskBinary
        if ($LASTEXITCODE) { throw "$taskCase compilation failed" }
        & $taskBinary
        if ($LASTEXITCODE) { throw "$taskCase failed" }
    }
} finally { Pop-Location }
