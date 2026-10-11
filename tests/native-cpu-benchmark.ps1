param(
  [Parameter(Mandatory)][string]$Loader,
  [Parameter(Mandatory)][string]$Circuit,
  [Parameter(Mandatory)][string]$VerilogMod,
  [int]$Seconds = 43,
  [string[]]$Modes = @('0','1','1','0'),
  # When supplied, mode 0 runs the previous loader with its JIT pass enabled.
  [string]$Baseline = '',
  [string]$ExpectedCandidateJitPattern = '',
  # Development-only scheduling control, applied to the cloned test process.
  [string]$CpuAffinity = ''
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskRun = Join-Path $taskRepo ('build\native-cpu-benchmark-' + [guid]::NewGuid().ToString('N'))
$taskPackageRoot = Join-Path $taskRun 'package'
$taskNative = Join-Path $taskPackageRoot 'native'
New-Item -ItemType Directory -Force $taskNative | Out-Null
$taskCompilerRoot = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
& (Join-Path $taskCompilerRoot 'g++.exe') -std=c++17 -O2 -static -shared `
  (Join-Path $PSScriptRoot 'native-cpu-benchmark-driver.cpp') -o (Join-Path $taskNative 'pin-shape-probe.dll')
if ($LASTEXITCODE) { throw 'CPU benchmark driver compilation failed' }
Set-Content -LiteralPath (Join-Path $taskPackageRoot 'mod.json') -Encoding ascii -Value @'
{"format":2,"id":"dev.pin-shape-probe","name":"Stock CPU benchmark","version":"0.1.0","capabilities":["log","hook","symbol","component","logic","game_handles"],"native":{"api":1,"entry":"native/pin-shape-probe.dll"}}
'@
Add-Type -AssemblyName System.IO.Compression.FileSystem
$taskPackage = Join-Path $taskRun 'dev.native-cpu-benchmark.mod'
[System.IO.Compression.ZipFile]::CreateFromDirectory($taskPackageRoot, $taskPackage)
$taskOldJit = $env:TC_MODLOADER_JIT_OPT
$taskOldAffinity = $env:TC_TEST_CPU_AFFINITY
$taskResults = @()
if ($Baseline -and (Get-FileHash -LiteralPath $Baseline).Hash -eq (Get-FileHash -LiteralPath $Loader).Hash) { throw 'Baseline and candidate loaders are identical' }
try {
  if ($CpuAffinity) { $env:TC_TEST_CPU_AFFINITY = $CpuAffinity }
  else { Remove-Item Env:TC_TEST_CPU_AFFINITY -ErrorAction SilentlyContinue }
  foreach ($taskMode in $Modes) {
    if ($taskMode -notin @('0','1')) { throw 'Modes must be 0 or 1' }
    $env:TC_MODLOADER_JIT_OPT = if ($Baseline) { '1' } else { $taskMode }
    $taskLoaderForRun = if ($Baseline -and $taskMode -eq '0') { $Baseline } else { $Loader }
    $taskOutput = @(& (Join-Path $PSScriptRoot 'repro-user-board.ps1') -Seconds $Seconds -Loader $taskLoaderForRun `
      -Circuit $Circuit -ProbeMod $taskPackage -VerilogMod $VerilogMod)
    $taskLine = $taskOutput | Where-Object { $_ -is [string] -and $_.StartsWith('Sandbox: ') } | Select-Object -Last 1
    if (!$taskLine) { throw 'Benchmark sandbox path missing' }
    $taskSandbox = $taskLine.Substring(9)
    $taskLog = Join-Path $taskSandbox 'game\tc-modloader-data\loader.log'
    if (Select-String -LiteralPath $taskLog -SimpleMatch 'CPUBENCH: FAIL') { throw "Benchmark cycle reset: $taskLog" }
    if ($CpuAffinity -and !(Select-String -LiteralPath $taskLog -SimpleMatch 'CPUBENCH: diagnostic process affinity applied')) { throw "Diagnostic affinity was not applied: $taskLog" }
    $taskOptimized = Select-String -LiteralPath $taskLog -Pattern 'Native simulation JIT: removed [1-9][0-9]* redundant instructions'
    if ((($Baseline -or $taskMode -eq '1') -and !$taskOptimized) -or (!$Baseline -and $taskMode -eq '0' -and $taskOptimized)) { throw "JIT mode did not match actual execution: $taskLog" }
    if ($taskMode -eq '1' -and $ExpectedCandidateJitPattern -and !(Select-String -LiteralPath $taskLog -Pattern $ExpectedCandidateJitPattern)) { throw "Candidate JIT transformation was not exercised: $taskLog" }
    $taskSamples = @(Select-String -LiteralPath $taskLog -Pattern 'CPUBENCH: t=([0-9.]+) cycle=(\d+)' | ForEach-Object {
      $taskMatch = [regex]::Match($_.Line, 'CPUBENCH: t=([0-9.]+) cycle=(\d+)')
      [pscustomobject]@{ t=[double]::Parse($taskMatch.Groups[1].Value,[cultureinfo]::InvariantCulture); cycle=[long]$taskMatch.Groups[2].Value }
    } | Where-Object { $_.t -ge 20 -and $_.t -le 37 })
    if ($taskSamples.Count -lt 10) { throw "Benchmark samples missing: $taskLog" }
    $taskFirst = $taskSamples[0]; $taskLast = $taskSamples[-1]
    $taskResult = [pscustomobject]@{ mode=$taskMode; loaderHash=(Get-FileHash -LiteralPath $taskLoaderForRun).Hash; diagnosticAffinity=$CpuAffinity; sandbox=$taskSandbox; first=$taskFirst; last=$taskLast; mhz=($taskLast.cycle-$taskFirst.cycle)/($taskLast.t-$taskFirst.t)/1000000 }
    $taskResults += $taskResult
    $taskResults | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $taskRun 'results.json')
    $taskResult | ConvertTo-Json -Depth 4 -Compress
  }
} finally { $env:TC_MODLOADER_JIT_OPT = $taskOldJit; $env:TC_TEST_CPU_AFFINITY = $taskOldAffinity }
"Results: $taskRun"
