# Real-machine oracle for one built-in kind (tests/kind-oracle-probe.cpp).
#
# Copies the game and the loader into an isolated sandbox, loads a probe whose
# only job is to put one built-in kind on the board through the command bus and
# dump what the game says about it, and asserts the dump.  The point is to answer
# the questions the game's own files cannot: which pins a kind really has, what
# it does with its configuration fields, and whether it can be placed at all.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests/kind-oracle-playtest.ps1 -Kind 0x2c
#
# `-Kind` may be given as 0x2c or 44.  The dump lands in build/kind-oracle-<name>/
# along with the loader log, so a failing run can be inspected afterwards.
param(
  [Parameter(Mandatory=$true)][string]$Kind,
  [int]$X = 0,
  [int]$Y = 0,
  [int]$Rotation = 0,
  [int]$Seconds = 40,
  # The level to open before placing.  The probe waits for a board, so the level
  # has to be one the game opens by itself: the sandbox is the default.
  [string]$Level = 'sandbox',
  # A level circuit to deploy as that level's board before starting the game.
  # tools/level-circuit.py writes these; the game then loads and runs OUR circuit,
  # which is how a built-in kind's behaviour gets measured without writing into
  # the game's memory.
  [string]$CircuitPath = $null,
  # Which campaign level the circuit and its level contract come from.  The meta
  # and test are what make the game drive and re-compute the board.
  [string]$LevelSource = $null,
  # Cycles the probe asks the game to run before it dumps the board's values.
  [int]$Run = 0,
  # Truth-table sweep: drive the level's global input at this board component
  # index through SweepFrom..SweepTo and record every wire's value each time.
  [int]$SweepInput = -1,
  [int]$SweepFrom = 0,
  [int]$SweepTo = 0
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskValue = if ($Kind -like '0x*') { [Convert]::ToInt32($Kind, 16) } else { [int]$Kind }
if ($taskValue -lt 1 -or $taskValue -gt 255) { throw "Kind must be 1..255 (got $Kind)" }
$taskProbe = Join-Path $taskRepo 'build\kind-oracle-probe.dll'
if (!(Test-Path -LiteralPath $taskProbe)) {
  throw "Missing probe DLL; build it first (see the header of tests/kind-oracle-probe.cpp): $taskProbe"
}
$taskLoader = Join-Path $taskRepo 'dist\tc-loader.dll'
if (!(Test-Path -LiteralPath $taskLoader)) { throw "Missing loader: $taskLoader (run build.ps1)" }

$taskName = ('kind-oracle-{0:x2}' -f $taskValue)
$taskTest = Join-Path $taskRepo ('build\' + $taskName)
if (Test-Path -LiteralPath $taskTest) { Remove-Item -LiteralPath $taskTest -Recurse -Force }
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot ('tc-modloader-data\plugin-data\test.kind-oracle')
$taskPackage = Join-Path $taskTest 'package'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods'),$taskData,(Join-Path $taskPackage 'native') | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath $taskLoader -Destination (Join-Path $taskRoot 'game_engine.dll')
if ($CircuitPath) {
  if (!(Test-Path -LiteralPath $CircuitPath)) { throw "Missing level circuit: $CircuitPath" }
  $taskLevelDir = Join-Path $taskRoot "campaign\$Level"
  New-Item -ItemType Directory -Force $taskLevelDir | Out-Null
  Copy-Item -LiteralPath $CircuitPath -Destination (Join-Path $taskLevelDir 'circuit.data') -Force
  # A level is more than its circuit: the game only drives and re-computes a board
  # when the level declares inputs/outputs and has a test to run it.  The sandbox's
  # own meta.txt/test.si are empty stubs, so a circuit dropped there is never
  # re-computed (measured: two different drive paths left every wire byte at 0).
  # `-LevelSource` names the campaign level whose meta/test go with the circuit.
  if ($LevelSource) {
    $taskSource = Join-Path $taskGame "campaign\$LevelSource"
    if (!(Test-Path -LiteralPath $taskSource)) { throw "No campaign level to copy from: $taskSource" }
    foreach ($taskExtra in @('meta.txt', 'test.si', 'ui.txt', 'label.txt', 'hint_solution.data')) {
      $taskFrom = Join-Path $taskSource $taskExtra
      if (Test-Path -LiteralPath $taskFrom) {
        Copy-Item -LiteralPath $taskFrom -Destination (Join-Path $taskLevelDir $taskExtra) -Force
      }
    }
    "Copied $LevelSource's meta/test/ui/label into campaign\$Level"
  } else {
    # Without a level contract the folder keeps the campaign's own meta.txt.
    $taskSourceMeta = Join-Path $taskGame "campaign\$Level\meta.txt"
    if ((Test-Path -LiteralPath $taskSourceMeta) -and !(Test-Path -LiteralPath (Join-Path $taskLevelDir 'meta.txt'))) {
      Copy-Item -LiteralPath $taskSourceMeta -Destination (Join-Path $taskLevelDir 'meta.txt')
    }
  }
  "Deployed $CircuitPath as campaign\$Level\circuit.data"
}
Copy-Item -LiteralPath $taskProbe -Destination (Join-Path $taskPackage 'native\probe.dll')
'{"format":2,"id":"test.kind-oracle","name":"Built-in kind oracle","version":"0.0.1","native":{"api":1,"entry":"native/probe.dll"}}' |
  Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding utf8
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRoot 'mods\probe.mod') | Out-Null
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.kind-oracle
if($LASTEXITCODE){throw 'Kind oracle package apply failed'}

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousKind = $env:TC_KIND_ORACLE_KIND
$taskPreviousX = $env:TC_KIND_ORACLE_X
$taskPreviousY = $env:TC_KIND_ORACLE_Y
$taskPreviousRotation = $env:TC_KIND_ORACLE_ROTATION
$taskPreviousRun = $env:TC_KIND_ORACLE_RUN
$taskPreviousSweep = $env:TC_KIND_ORACLE_SWEEP_INPUT
$taskPreviousSweepFrom = $env:TC_KIND_ORACLE_SWEEP_FROM
$taskPreviousSweepTo = $env:TC_KIND_ORACLE_SWEEP_TO
$taskProcess = $null
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $env:TC_KIND_ORACLE_KIND = [string]$taskValue
  $env:TC_KIND_ORACLE_X = [string]$X
  $env:TC_KIND_ORACLE_Y = [string]$Y
  $env:TC_KIND_ORACLE_ROTATION = [string]$Rotation
  if ($Run -gt 0) { $env:TC_KIND_ORACLE_RUN = [string]$Run }
  if ($SweepInput -ge 0) {
    $env:TC_KIND_ORACLE_SWEEP_INPUT = [string]$SweepInput
    $env:TC_KIND_ORACLE_SWEEP_FROM = [string]$SweepFrom
    $env:TC_KIND_ORACLE_SWEEP_TO = [string]$SweepTo
  }
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  $taskResult = Join-Path $taskData 'result.txt'
  while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
    Start-Sleep -Milliseconds 250
  }
  if(!(Test-Path -LiteralPath $taskResult)) {
    $taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
    if(Test-Path -LiteralPath $taskLog) { Get-Content -LiteralPath $taskLog -Tail 20 }
    throw "Kind oracle did not finish; inspect $taskRoot\tc-modloader-data\loader.log"
  }
  $taskText = [IO.File]::ReadAllText($taskResult)
  $taskText
  if($taskText.StartsWith('PASS ')) {
    # The dump is the deliverable: keep it next to the package.
    Copy-Item -LiteralPath $taskResult -Destination (Join-Path $taskTest 'board-dump.txt') -Force
    "Evidence: $taskTest"
  } else {
    throw "Kind oracle failed for kind 0x$('{0:x2}' -f $taskValue): $taskText"
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:TC_KIND_ORACLE_KIND = $taskPreviousKind
  $env:TC_KIND_ORACLE_X = $taskPreviousX
  $env:TC_KIND_ORACLE_Y = $taskPreviousY
  $env:TC_KIND_ORACLE_ROTATION = $taskPreviousRotation
  $env:TC_KIND_ORACLE_RUN = $taskPreviousRun
  $env:TC_KIND_ORACLE_SWEEP_INPUT = $taskPreviousSweep
  $env:TC_KIND_ORACLE_SWEEP_FROM = $taskPreviousSweepFrom
  $env:TC_KIND_ORACLE_SWEEP_TO = $taskPreviousSweepTo
  if($taskProcess -and !$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
  }
}
