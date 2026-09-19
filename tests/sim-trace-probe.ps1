# Development-only probe: locate the per-cycle level I/O trace.
#
# Runs the stock and_gate level in an isolated sandbox, steps it cycle by cycle
# and dumps the two history buffers plus the indexed state reads.  The report
# lands in the plugin's data directory as trace-map.txt; this script just makes
# the run reproducible and prints where to look.
param([int]$Seconds = 45)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskPackage = Join-Path $taskRepo 'build\sim-trace-probe-package'
$taskMod = Join-Path $taskRepo 'dist\dev.sim-trace-probe.mod'
$taskSandbox = Join-Path $taskRepo 'build\sim-trace-sandbox'
$taskRoot = Join-Path $taskSandbox 'game'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native') | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk `
  (Join-Path $PSScriptRoot 'sim-trace-probe.cpp') -o (Join-Path $taskPackage 'native\trace.dll')
if ($LASTEXITCODE) { throw 'Trace probe compilation failed' }
'{"format":2,"id":"dev.sim-trace-probe","name":"Sim trace probe","version":"0.1.0","native":{"api":1,"entry":"native/trace.dll"}}' |
  Set-Content (Join-Path $taskPackage 'mod.json') -Encoding ascii
if (Test-Path -LiteralPath $taskMod) { Remove-Item -LiteralPath $taskMod }
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output $taskMod
& (Join-Path $PSScriptRoot 'make-ui-sandbox.ps1') -Sandbox $taskSandbox -Mods dev.sim-trace-probe
# The level's own test needs a circuit to test, and a fresh sandbox has none:
# install the built-in AND solution into the profile's and_gate slot exactly
# like tests/sim-state-playtest.ps1 does.  and_gate's test feeds inputs
# 0,1,2,3 and expects outputs 0,0,0,1 - the sequence this probe is looking for.
$taskSchema = Join-Path $taskSandbox 'home\AppData\Roaming\Turing Complete Mods\profiles\default\schematics\and_gate\Default'
$taskLevel = if ($env:TC_TRACE_LEVEL) { $env:TC_TRACE_LEVEL } else { 'and_gate' }
$taskSchema = Join-Path $taskSandbox ("home\AppData\Roaming\Turing Complete Mods\profiles\default\schematics\" + $taskLevel + "\Default")
New-Item -ItemType Directory -Force $taskSchema | Out-Null
$taskSolution = if ($env:TC_TRACE_SOLUTION) {
  Join-Path $taskRepo $env:TC_TRACE_SOLUTION
} else {
  Join-Path $taskRepo 'build\and2_solution_builtin.data'
}
if (!(Test-Path -LiteralPath $taskSolution)) { throw "Missing $taskSolution; run build.ps1 first" }
Copy-Item -LiteralPath $taskSolution -Destination (Join-Path $taskSchema 'circuit.data') -Force
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.sim-trace-probe
if ($LASTEXITCODE) { throw 'Trace probe package apply failed' }
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
Remove-Item -LiteralPath $taskLog -ErrorAction SilentlyContinue
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskProcess = $null
try {
    $env:USERPROFILE = Join-Path $taskSandbox 'home'
    $env:APPDATA = Join-Path $env:USERPROFILE 'AppData\Roaming'
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
      -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline = (Get-Date).AddSeconds($Seconds)
    do {
        Start-Sleep -Milliseconds 500
        $taskText = if (Test-Path -LiteralPath $taskLog) { Get-Content -LiteralPath $taskLog -Raw } else { '' }
        if ($taskText -match 'TRACE DONE|Native failed|Plugin callback threw' -or $taskProcess.HasExited) { break }
    } while ((Get-Date) -lt $taskDeadline)
} finally {
    $env:USERPROFILE = $taskPreviousProfile
    $env:APPDATA = $taskPreviousAppData
    if ($taskProcess -and !$taskProcess.HasExited) {
        [void]$taskProcess.CloseMainWindow()
        if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id }
    }
}
$taskText = Get-Content -LiteralPath $taskLog -Raw
if ($taskText -notmatch 'TRACE DONE') {
    Get-Content -LiteralPath $taskLog -Tail 20
    throw 'The trace probe did not finish'
}
$taskMap = Get-ChildItem -Path $taskRoot -Recurse -Filter 'trace-map.txt' -ErrorAction SilentlyContinue |
  Select-Object -First 1
if (!$taskMap) { throw 'The trace probe did not write trace-map.txt' }
"Trace report: $($taskMap.FullName)"
# The SDK reader has to reproduce the level's own test sequence: and_gate feeds
# inputs 0,1,2,3 and expects outputs 0,0,0,1.
$taskRows = Select-String -LiteralPath $taskLog -Pattern 'trace: row \d+ cycle=-?\d+ in=([\d,]+) out=([\d,]+)' |
  ForEach-Object { $_.Matches[0].Groups[1].Value + ' -> ' + $_.Matches[0].Groups[2].Value }
if ($taskRows.Count -lt 3) {
  Get-Content -LiteralPath $taskLog -Tail 20
  throw "The sampler produced only $($taskRows.Count) rows"
}
"Sampler rows: $($taskRows -join ' | ')"
$taskVcd = Get-ChildItem -Path $taskRoot -Recurse -Filter 'trace.vcd' -ErrorAction SilentlyContinue |
  Select-Object -First 1
if (!$taskVcd) { throw 'The sampler did not write trace.vcd' }
"VCD: $($taskVcd.FullName) ($($taskVcd.Length) bytes)"
$taskSampler = Select-String -LiteralPath $taskLog -Pattern 'trace: sampler inputs=(\d+) outputs=(\d+) rows=(\d+) assumedStride=(\d)' |
  Select-Object -First 1
if (!$taskSampler) { throw 'The sampler never reported its slot counts' }
"$($taskSampler.Line)"
Get-Content -LiteralPath $taskMap.FullName | Select-Object -First 20
"PASS sim trace: the SDK sampler resolved the level's I/O slots, reproduced its test sequence and wrote a VCD"
