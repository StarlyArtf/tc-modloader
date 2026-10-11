# Compare the same frozen stock CPU at every cycle, using worker-thread capture.
param(
  [Parameter(Mandatory)][string]$Loader,
  [Parameter(Mandatory)][string]$Baseline,
  [Parameter(Mandatory)][string]$Circuit,
  [Parameter(Mandatory)][string]$VerilogMod,
  [string]$ExpectedCandidateJitPattern = ''
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskRun = Join-Path $taskRepo ('build\native-cpu-state-' + [guid]::NewGuid().ToString('N'))
$taskPackageRoot = Join-Path $taskRun 'package'
$taskNative = Join-Path $taskPackageRoot 'native'
New-Item -ItemType Directory -Force $taskNative | Out-Null
$taskCompilerRoot = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
& (Join-Path $taskCompilerRoot 'g++.exe') -std=c++17 -O2 -static -shared `
  (Join-Path $PSScriptRoot 'native-cpu-state-driver.cpp') -o (Join-Path $taskNative 'pin-shape-probe.dll')
if ($LASTEXITCODE) { throw 'CPU state driver compilation failed' }
Set-Content -LiteralPath (Join-Path $taskPackageRoot 'mod.json') -Encoding ascii -Value @'
{"format":2,"id":"dev.pin-shape-probe","name":"Stock CPU cycle state probe","version":"0.1.0","capabilities":["log","hook","symbol","component","logic","game_handles"],"native":{"api":1,"entry":"native/pin-shape-probe.dll"}}
'@
Add-Type -AssemblyName System.IO.Compression.FileSystem
$taskPackage = Join-Path $taskRun 'dev.native-cpu-state.mod'
[System.IO.Compression.ZipFile]::CreateFromDirectory($taskPackageRoot, $taskPackage)
$taskStateFiles = @()
$taskOldJit = $env:TC_MODLOADER_JIT_OPT
try {
$env:TC_MODLOADER_JIT_OPT = '1'
foreach ($taskBinary in @($Baseline, $Loader)) {
  $taskOutput = @(& (Join-Path $PSScriptRoot 'repro-user-board.ps1') -Seconds 26 -Loader $taskBinary `
    -Circuit $Circuit -ProbeMod $taskPackage -VerilogMod $VerilogMod)
  $taskSandboxLine = $taskOutput | Where-Object { $_ -is [string] -and $_.StartsWith('Sandbox: ') } | Select-Object -Last 1
  if (!$taskSandboxLine) { throw 'CPU state sandbox path missing' }
  $taskSandbox = $taskSandboxLine.Substring(9)
  $taskLog = Join-Path $taskSandbox 'game\tc-modloader-data\loader.log'
  if (!(Select-String -LiteralPath $taskLog -SimpleMatch 'CPUSTATE: PASS rows=4096 channels=64 final=4095 gaps=0')) {
    throw "CPU state assertion failed: $taskLog"
  }
  if ($taskStateFiles.Count -eq 1 -and !(Select-String -LiteralPath $taskLog -Pattern 'Native simulation JIT: removed [1-9][0-9]* redundant instructions')) {
    throw "Candidate did not optimize JIT code: $taskLog"
  }
  if ($taskStateFiles.Count -eq 1 -and $ExpectedCandidateJitPattern -and !(Select-String -LiteralPath $taskLog -Pattern $ExpectedCandidateJitPattern)) {
    throw "Candidate JIT transformation was not exercised: $taskLog"
  }
  $taskStateFile = Join-Path $taskSandbox 'game\tc-modloader-data\plugin-data\dev.pin-shape-probe\cpu-state.csv'
  $taskRows = @(Get-Content -LiteralPath $taskStateFile)
  if ($taskRows.Count -ne 4096) { throw "Incomplete state file: $taskStateFile" }
  for ($taskRow=0; $taskRow -lt 4096; ++$taskRow) {
    if ($taskRows[$taskRow] -notmatch ('^' + $taskRow + '(,[0-9a-f]{16}){64}$')) { throw "Invalid state row $taskRow in $taskStateFile" }
  }
  $taskStateFiles += $taskStateFile
}
} finally { $env:TC_MODLOADER_JIT_OPT = $taskOldJit }
$taskHashes = @($taskStateFiles | ForEach-Object { (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash })
if ($taskHashes[0] -ne $taskHashes[1]) { throw "CPU states differ: $($taskStateFiles -join ', ')" }
"PASS stock CPU: 4096 cycles x 64 state channels identical; SHA256=$($taskHashes[0])"
$taskStateFiles
