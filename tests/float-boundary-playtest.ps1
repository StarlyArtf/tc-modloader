param(
  [int]$Seconds = 120,
  [switch]$KeepRunning
)

# Diagnostic for the loader's known wide-output boundary
# (docs/reference/limits.md): word outputs are published into one 512-byte state
# slot per binding token, and only the first 32 tokens have their own slot.
# This case therefore *records* where a 42-instance Float Ops chain stops
# carrying 0xDEADBEEF instead of demanding that the whole board work: tokens
# 1..32 must hold the pattern, and the first token that does not is the boundary
# the M0 report has to state.  It never fails because of that boundary - it fails
# only when the documented range itself breaks or the run cannot be driven.

$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskCxx = Join-Path $taskCompiler 'g++.exe'
$taskId = [guid]::NewGuid().ToString('N')
$taskDriverBuild = Join-Path $taskRepo ('build\float-boundary-driver-' + $taskId)
$taskDriverStage = Join-Path $taskDriverBuild 'package'
$taskDriverNative = Join-Path $taskDriverStage 'native'
$taskDriverMod = Join-Path $taskDriverBuild 'local.float-ops.mod'
$taskBoard = Join-Path $taskDriverBuild 'float-boundary-board.data'
$taskReport = Join-Path $taskRepo 'build\float-boundary-report.txt'
$taskSandbox = Join-Path $taskRepo ('build\float-boundary-' + $taskId)
$taskRoot = Join-Path $taskSandbox 'game'
$taskProfile = Join-Path $taskSandbox 'home'
$taskMods = Join-Path $taskRoot 'mods'
$taskLoader = Join-Path $taskRepo 'dist\tc-loader.dll'
$taskCli = Join-Path $taskRepo 'dist\tcmod-cli.exe'
$taskLevel = 'not_gate'
$taskSchema = Join-Path $taskProfile `
  "AppData\Roaming\Turing Complete Mods\profiles\default\schematics\$taskLevel\Default"
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\local.float-ops'

foreach ($taskRequired in @($taskCxx, $taskLoader, $taskCli,
                             (Join-Path $taskGame 'Turing Complete.exe'))) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}

New-Item -ItemType Directory -Force $taskDriverNative | Out-Null
# M1: the plugin object calls the binary32 kernel, so the same vendored
# SoftFloat objects the Mod package links have to go into the test driver.
& (Join-Path $taskRepo 'examples\float-ops\build-kernel.ps1') | Out-Host
if ($LASTEXITCODE) { throw 'Float Ops kernel build failed' }
$taskKernelObjects = @(Get-Content -LiteralPath `
  (Join-Path $taskRepo 'build\float-ops\float-kernel-objects.rsp'))
$taskKernelFlags = @(Get-Content -LiteralPath `
  (Join-Path $taskRepo 'build\float-ops\float-kernel-includes.rsp'))

$taskFloatObject = Join-Path $taskDriverBuild 'float-ops.o'
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror `
  -Dtc_mod_load=float_ops_m0_embedded_load "-I$(Join-Path $taskRepo 'sdk')" `
  @taskKernelFlags -c `
  (Join-Path $taskRepo 'examples\float-ops\plugin.cpp') -o $taskFloatObject
if ($LASTEXITCODE) { throw 'Float Ops embedded test object did not compile' }

$taskDriverDll = Join-Path $taskDriverNative 'float-compat-driver.dll'
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -Wno-unused-function `
  -Wno-unused-variable -static -static-libgcc -static-libstdc++ -shared `
  "-I$(Join-Path $taskRepo 'sdk')" (Join-Path $PSScriptRoot 'float-compat-driver.cpp') `
  $taskFloatObject $taskKernelObjects -o $taskDriverDll
if ($LASTEXITCODE) { throw 'Float Ops true-game driver did not compile' }

$taskManifest = @'
{
  "format": 2,
  "id": "local.float-ops",
  "name": "Float Ops M0 boundary driver",
  "version": "0.1.0",
  "capabilities": ["log", "status", "services", "game_handles", "symbol", "hook", "logic", "component"],
  "native": {"api": 1, "entry": "native/float-compat-driver.dll"}
}
'@
Set-Content -LiteralPath (Join-Path $taskDriverStage 'mod.json') -Value $taskManifest -Encoding ascii
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskDriverStage -Output $taskDriverMod | Out-Null

$taskFixtureExe = Join-Path $taskDriverBuild 'float-fixture.exe'
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static `
  (Join-Path $PSScriptRoot 'float-fixture.cpp') -o $taskFixtureExe
if ($LASTEXITCODE) { throw 'Float Ops fixture generator did not compile' }
& $taskFixtureExe --boundary $taskBoard | Out-Host
if ($LASTEXITCODE -or !(Test-Path -LiteralPath $taskBoard)) {
  throw 'Float Ops boundary fixture was not generated'
}

New-Item -ItemType Directory -Force $taskRoot,$taskProfile,$taskMods,$taskSchema,$taskData | Out-Null
foreach ($taskDir in @('asset', 'campaign', 'translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll',
                         'soft_oal.dll', 'steam_api64.dll', 'libgcc_s_seh-1.dll',
                         'libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath $taskLoader -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath $taskDriverMod -Destination (Join-Path $taskMods 'local.float-ops.mod')
Copy-Item -LiteralPath $taskBoard -Destination (Join-Path $taskSchema 'circuit.data')
Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value $taskLevel -Encoding ascii

& $taskCli $taskRoot apply local.float-ops | Out-Host
if ($LASTEXITCODE) { throw 'Float Ops isolated apply failed' }

$taskDoneMarker = 'float-compat: post-reset cycles reached'
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskProcess = $null
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  do {
    Start-Sleep -Milliseconds 500
    $taskText = if (Test-Path -LiteralPath $taskLog) {
      Get-Content -LiteralPath $taskLog -Raw
    } else { '' }
    if ($taskText.Contains($taskDoneMarker) -or
        $taskText.Contains('float-ops M0: registration failed') -or
        $taskText.Contains('Native failed') -or $taskProcess.HasExited) { break }
  } while ([DateTime]::UtcNow -lt $taskDeadline)
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) {
      Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue
    }
  }
}

if (!(Test-Path -LiteralPath $taskLog)) { throw "No loader log; inspect $taskSandbox" }
$taskLines = Get-Content -LiteralPath $taskLog
$taskText = $taskLines -join "`n"
if (!$taskText.Contains('float-ops M0: registered 32-bit source/pass/result+flags/flags-sink/word-sink probes; all are stateless')) {
  throw "The boundary board did not register all five Float Ops definitions; inspect $taskLog"
}
if (!$taskText.Contains($taskDoneMarker)) {
  throw "The boundary run never reached its post-reset cycles; inspect $taskLog"
}

# M1: the same check the compat case makes - the kernel links into the Mod DLL
# and computes under the real loader.
$taskKernelLine = 'float-ops M1: kernel self-check 1+2=0x40400000/0x00 ' +
  '(1+ulp)^2=0x3F800002/0x01 fma=0x34000001/0x00 1/0=0x7F800000/0x08 ' +
  'sqrt(-1)=0x7FC00000/0x10'
if (!$taskText.Contains($taskKernelLine)) {
  throw "The binary32 kernel did not report the expected self-check inside the game; inspect $taskLog"
}

# token -> instance id, straight out of the loader's own binding log.
$taskTokenInstance = @{}
foreach ($taskLine in $taskLines) {
  $taskBound = [regex]::Match($taskLine,
    'Native logic: bound instance (0x[0-9a-f]+) of custom (0x[0-9a-f]+) as token (\d+)')
  if ($taskBound.Success) {
    $taskTokenInstance[[int]$taskBound.Groups[3].Value] = $taskBound.Groups[1].Value
  }
}
# instance id -> the first value its callback observed in a cycle.
$taskInstanceValue = @{}
foreach ($taskLine in $taskLines) {
  $taskObserved = [regex]::Match($taskLine,
    'float-ops M0: (\S+) cycle observed, instance=(0x[0-9a-f]+) cycle=(-?\d+) value=(0x[0-9a-f]+)')
  if (!$taskObserved.Success) { continue }
  $taskKey = $taskObserved.Groups[2].Value
  if (!$taskInstanceValue.ContainsKey($taskKey)) {
    $taskInstanceValue[$taskKey] = @{
      Label = $taskObserved.Groups[1].Value
      Value = $taskObserved.Groups[4].Value
    }
  }
}

$taskExpected = 42
if ($taskTokenInstance.Count -lt $taskExpected) {
  throw ("Only {0} of {1} Float Ops instances bound; the boundary case needs the whole chain" -f `
         $taskTokenInstance.Count, $taskExpected)
}

$taskReliable = 32
$taskFirstMismatch = -1
$taskReportLines = @(
  'Float Ops M0 binding-token boundary',
  ('board: source -> pass x38 -> result+flags -> sink (+ flags sink), {0} instances' -f $taskTokenInstance.Count),
  ('documented reliable range: tokens 1..{0} (docs/reference/limits.md)' -f $taskReliable),
  'the value below is what the probe read as its input: a pass probe at token N reads',
  'the wide output published by token N-1, so a divergence at token N means token N-1',
  'had no slot to publish into.',
  '',
  'token instance            probe         input       expected',
  '--------------------------------------------------------------------------------'
)
foreach ($taskToken in ($taskTokenInstance.Keys | Sort-Object)) {
  $taskInstance = $taskTokenInstance[$taskToken]
  $taskObserved = $taskInstanceValue[$taskInstance]
  $taskValue = if ($taskObserved) { $taskObserved.Value } else { '<no observation>' }
  $taskLabel = if ($taskObserved) { $taskObserved.Label } else { '<none>' }
  $taskWant = if ($taskLabel -eq 'flags-sink') { '0x0000001f' } else { '0xdeadbeef' }
  $taskMatches = $taskValue -eq $taskWant
  if (!$taskMatches -and $taskFirstMismatch -lt 0) { $taskFirstMismatch = $taskToken }
  $taskReportLines += ('{0,5} {1}  {2,-12} {3}  {4}{5}' -f `
    $taskToken, $taskInstance, $taskLabel, $taskValue, $taskWant,
    $(if ($taskMatches) { '' } else { '   <-- diverges' }))
}
$taskReportLines += ''
if ($taskFirstMismatch -lt 0) {
  $taskReportLines += ('result: every token 1..{0} carried the expected pattern; the documented 32-token limit was not reached by this board' -f $taskTokenInstance.Count)
} else {
  $taskReportLines += ('result: the first probe that read a wrong value was token {0}; its input comes from token {1}, so token {1} is the first wide output the loader did not publish' -f `
    $taskFirstMismatch, ($taskFirstMismatch - 1))
}
Set-Content -LiteralPath $taskReport -Value $taskReportLines -Encoding utf8
$taskReportLines | ForEach-Object { $_ }

# Tokens 1..32 are the range the loader claims; a failure there would be a real
# compatibility blocker, so this case does fail on it.  Everything above the
# boundary is recorded, not required.
$taskUnverified = @()
foreach ($taskToken in 1..$taskReliable) {
  if (!$taskTokenInstance.ContainsKey($taskToken)) {
    throw "Token $taskToken was never bound; the boundary case cannot judge the reliable range"
  }
  $taskObserved = $taskInstanceValue[$taskTokenInstance[$taskToken]]
  $taskValue = if ($taskObserved) { $taskObserved.Value } else { '' }
  if ($taskValue -ne '0xdeadbeef') {
    $taskUnverified += ('token {0}={1}' -f $taskToken, $(if ($taskValue) { $taskValue } else { '<none>' }))
  }
}
if ($taskUnverified.Count) {
  throw ('Tokens inside the documented 32-token range did not carry the pattern: ' +
         ($taskUnverified -join ', '))
}

$taskBoundMessage = if ($taskFirstMismatch -lt 0) {
  'the whole 42-instance chain carried the pattern'
} else {
  "token $($taskFirstMismatch - 1) is the first wide output without a slot (its consumer, token $taskFirstMismatch, read 0 instead)"
}
"PASS float-ops boundary diagnostic: tokens 1..$taskReliable hold 0xDEADBEEF; $taskBoundMessage"
"Report: $taskReport"
"Sandbox: $taskSandbox"
