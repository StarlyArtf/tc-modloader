param(
  [int]$Seconds = 90,
  [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskCxx = Join-Path $taskCompiler 'g++.exe'
$taskId = [guid]::NewGuid().ToString('N')
$taskDriverBuild = Join-Path $taskRepo ('build\float-compat-driver-' + $taskId)
$taskDriverStage = Join-Path $taskDriverBuild 'package'
$taskDriverNative = Join-Path $taskDriverStage 'native'
$taskDriverMod = Join-Path $taskDriverBuild 'local.float-ops.mod'
$taskBoard = Join-Path $taskDriverBuild 'float-compat-board.data'
$taskSandbox = Join-Path $taskRepo ('build\float-compat-' + $taskId)
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

# The driver package contains the real Float Ops plugin object plus a test-only
# level runner.  The production local.float-ops.mod never contains the runner.
# M1: the plugin object calls the binary32 kernel, so the same vendored
# SoftFloat objects the Mod package links have to go into the test driver.
& (Join-Path $taskRepo 'examples\float-ops\build-kernel.ps1') | Out-Host
if ($LASTEXITCODE) { throw 'Float Ops kernel build failed' }
$taskKernelObjects = @(Get-Content -LiteralPath `
  (Join-Path $taskRepo 'build\float-ops\float-kernel-objects.rsp'))
$taskKernelFlags = @(Get-Content -LiteralPath `
  (Join-Path $taskRepo 'build\float-ops\float-kernel-includes.rsp'))

New-Item -ItemType Directory -Force $taskDriverNative | Out-Null
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
  "name": "Float Ops M0 true-game driver",
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
& $taskFixtureExe $taskBoard | Out-Host
if ($LASTEXITCODE -or !(Test-Path -LiteralPath $taskBoard)) {
  throw 'Float Ops fixture was not generated'
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

# Every phase the M0 checklist names leaves its own line: the first cycles of a
# reset segment (one line per instance, so a token can be matched with the value
# it carried), the UI refresh path of the paused board, the reset and the run
# the driver issues afterwards.
$taskProbes = @(
  @{Name = 'source'; Value = '0xdeadbeef'},
  @{Name = 'pass'; Value = '0xdeadbeef'},
  @{Name = 'result-flags'; Value = '0xdeadbeef'},
  @{Name = 'flags-sink'; Value = '0x0000001f'},
  @{Name = 'sink'; Value = '0xdeadbeef'}
)
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
    $taskAllCallbacks = $taskText.Contains($taskDoneMarker)
    foreach ($taskProbe in $taskProbes) {
      if (!$taskText.Contains("float-ops M0: $($taskProbe.Name) cycle observed") -or
          !$taskText.Contains("float-ops M0: $($taskProbe.Name) refresh observed") -or
          !$taskText.Contains("float-ops M0: $($taskProbe.Name) reset #")) {
        $taskAllCallbacks = $false
      }
    }
    if ($taskAllCallbacks -or $taskText.Contains('float-ops M0: registration failed') -or
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
$taskEvidence = @($taskLines | Where-Object {
  $_ -match 'float-ops M0:|float-compat:|Native loaded: local.float-ops|Native logic: (registered|bound instance)'
})
$taskEvidence | Select-Object -First 60 | ForEach-Object { $_ }

function Find-LineIndex([string[]]$Lines, [string]$Needle, [int]$From = 0) {
  for ($i = $From; $i -lt $Lines.Count; ++$i) {
    if ($Lines[$i].Contains($Needle)) { return $i }
  }
  return -1
}

$taskSuccess = 'float-ops M0: registered 32-bit source/pass/result+flags/flags-sink/word-sink probes; all are stateless'
if (!$taskText.Contains($taskSuccess)) {
  throw "The game did not accept all five Float Ops definitions; inspect $taskLog"
}
if ($taskText.Contains('float-ops M0: registration failed') -or
    $taskText.Contains('Native failed')) {
  throw "Float Ops reported a native registration failure; inspect $taskLog"
}

# M1: the kernel is inside the shipped DLL and runs under the real loader.
$taskKernelLine = 'float-ops M1: kernel self-check 1+2=0x40400000/0x00 ' +
  '(1+ulp)^2=0x3F800002/0x01 fma=0x34000001/0x00 1/0=0x7F800000/0x08 ' +
  'sqrt(-1)=0x7FC00000/0x10'
if (!$taskText.Contains($taskKernelLine)) {
  throw "The binary32 kernel did not report the expected self-check inside the game; inspect $taskLog"
}

foreach ($taskProbe in $taskProbes) {
  $taskName = $taskProbe.Name
  $taskValue = $taskProbe.Value
  $taskExpected = "float-ops M0: $taskName cycle observed"
  $taskMatching = @($taskLines | Where-Object { $_.Contains($taskExpected) })
  if (!$taskMatching.Count) {
    throw "Missing true-game cycle evidence for $taskName; inspect $taskLog"
  }
  foreach ($taskLine in $taskMatching) {
    if ($taskLine -notmatch "value=$taskValue") {
      throw "$taskName observed the wrong value in a cycle: $taskLine"
    }
  }

  $taskRefresh = @($taskLines | Where-Object {
    $_.Contains("float-ops M0: $taskName refresh observed")
  })
  if (!$taskRefresh.Count) {
    throw "Missing true-game refresh evidence for $taskName; inspect $taskLog"
  }
  foreach ($taskLine in $taskRefresh) {
    if ($taskLine -notmatch "value=$taskValue") {
      throw "$taskName observed the wrong value while refreshing: $taskLine"
    }
  }

  $taskReset = @($taskLines | Where-Object { $_.Contains("float-ops M0: $taskName reset #") })
  if (!$taskReset.Count) {
    throw "Missing true-game reset evidence for $taskName; inspect $taskLog"
  }
}

# The five-bit output is not just allocated: the sink reads the pattern the dual
# probe published on its second output.
$taskFlagsLine = @($taskLines | Where-Object {
  $_.Contains('float-ops M0: flags-sink cycle observed')
}).Count
if (!$taskFlagsLine) { throw 'The Flags[5] output never reached its sink' }

$taskCompiled = Find-LineIndex $taskLines 'float-compat: board compiled'
$taskTookOver = Find-LineIndex $taskLines 'float-compat: driver took over the simulation'
$taskPaused = Find-LineIndex $taskLines 'float-compat: paused after cycle='
$taskResetIssued = Find-LineIndex $taskLines 'float-compat: reset issued'
$taskResumed = Find-LineIndex $taskLines 'float-compat: resumed after reset'
$taskReached = Find-LineIndex $taskLines $taskDoneMarker
foreach ($taskStage in @(
    @{Index = $taskCompiled; Name = 'compile'},
    @{Index = $taskTookOver; Name = 'take-over'},
    @{Index = $taskPaused; Name = 'pause'},
    @{Index = $taskResetIssued; Name = 'reset'},
    @{Index = $taskResumed; Name = 'resume'},
    @{Index = $taskReached; Name = 'post-reset run'})) {
  if ($taskStage.Index -lt 0) {
    throw "The driver never reported its $($taskStage.Name) step; inspect $taskLog"
  }
}
if (!($taskCompiled -lt $taskTookOver -and $taskTookOver -lt $taskPaused -and
      $taskPaused -lt $taskResetIssued -and $taskResetIssued -lt $taskResumed -and
      $taskResumed -lt $taskReached)) {
  throw "The driver's pause/reset/run order is broken; inspect $taskLog"
}

# A pause that only stops the simulation still has to show the right values: the
# refresh path runs between the pause and the reset, and every probe reports it.
foreach ($taskProbe in $taskProbes) {
  $taskPausedRefresh = Find-LineIndex $taskLines `
    "float-ops M0: $($taskProbe.Name) refresh observed" $taskPaused
  if ($taskPausedRefresh -lt 0 -or $taskPausedRefresh -gt $taskResetIssued) {
    throw "No refresh observation for $($taskProbe.Name) during the pause window; inspect $taskLog"
  }
}

# Reset is followed by real cycles, and those cycles still carry the pattern.
foreach ($taskProbe in $taskProbes) {
  $taskPostReset = Find-LineIndex $taskLines `
    "float-ops M0: $($taskProbe.Name) cycle observed" $taskResumed
  if ($taskPostReset -lt 0) {
    throw "No post-reset cycle for $($taskProbe.Name); inspect $taskLog"
  }
  if ($taskLines[$taskPostReset] -notmatch "value=$($taskProbe.Value)") {
    throw "A post-reset cycle for $($taskProbe.Name) carried the wrong value: " +
          $taskLines[$taskPostReset]
  }
}

# The reset summary line carries what the segment before it looked like: a
# nonzero mismatch count would mean a probe saw a value its definition cannot
# produce on this board.
$taskResetSummaries = @($taskLines | Where-Object { $_ -match 'float-ops M0: .* reset #\d+' })
if (!$taskResetSummaries.Count) { throw 'No reset summary lines were written' }
foreach ($taskLine in $taskResetSummaries) {
  $taskMismatch = [regex]::Match($taskLine, 'mismatched=(\d+)')
  if (!$taskMismatch.Success) {
    throw "A reset summary did not report the mismatch count: $taskLine"
  }
  if ([int]$taskMismatch.Groups[1].Value -ne 0) {
    throw "A probe observed a value it cannot produce: $taskLine"
  }
}

$taskBound = @($taskLines | Where-Object { $_ -match 'Native logic: bound instance' }).Count
if ($taskBound -lt 21) { throw "Only $taskBound Float Ops instances bound; expected at least 21" }
foreach ($taskToken in 1..21) {
  $taskTokenPattern = 'as token ' + $taskToken + '$'
  if (!@($taskLines | Where-Object { $_ -match $taskTokenPattern }).Count) {
    throw "Binding token $taskToken was never assigned; inspect $taskLog"
  }
}

"PASS float-ops true-game runtime: 21 instances bound (tokens 1..21), 0xDEADBEEF reached source/pass/result+flags/sink callbacks, the Flags[5] output reached its own sink, refresh rendered the values while paused, and the post-reset cycles still carried them"
"Sandbox: $taskSandbox"
