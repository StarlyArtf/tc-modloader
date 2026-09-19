# Real-machine playtest for the Board handle registry (sdk/tc_handle_api.h).
#
# tests/game-handles.cpp proves the registry in isolation; this playtest proves
# the half only the real game can produce - a level lifetime.  The driver build
# of tests/game-handle-probe.cpp presses a home-page entry to enter a board,
# loads a second level with the game's own load_level() and finally presses
# Escape, which is the player's own way out; the plugin reports the registry
# through the public ABI only (it never hooks anything itself):
#
#   * the main menu has no Board: the query has to answer "unavailable";
#   * entering a level issues a handle whose resolve() is exactly the board the
#     LEVEL_LOAD event carries, and validate() says 1;
#   * the reserved kinds stay reserved: asking for a COMPONENT handle answers
#     TC_HANDLE_ERR_KIND on a real loader, not a fake handle;
#   * leaving the level - by the game's own exit, not by a scene change the
#     driver invents - invalidates the old handle (validate() 0, resolve()
#     STALE) and the query goes back to "unavailable": the Escape key makes the
#     game call change_scene itself, which is the path a player takes, and the
#     loader's detour invalidates before it raises the event;
#   * entering the level a second time gets a different generation and token, so a
#     handle kept from the first visit can never resolve into the second one.
#     Both visits use the campaign screen's own entry, because the sandbox save
#     has nothing else unlocked: measured with TC_HANDLE_PROBE_TRACE=1, entries
#     #3/#4 open the level's own screen (no LEVEL_LOAD) and #5 makes the game exit
#     by itself.
#
# -Example runs the read-only dev.game-handle-probe instead: it only proves that
# the published package loads and reports the registry from the menu.
param(
  [string]$Sandbox = (Join-Path (Split-Path $PSScriptRoot) 'build\game-handle-sandbox'),
  [int]$Seconds = 120,
  [switch]$Example,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskRoot = Join-Path $Sandbox 'game'
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskMod = if ($Example) { 'dev.game-handle-probe' } else { 'dev.game-handle-probe-driver' }

# Always refresh the sandbox: make-ui-sandbox.ps1 is what copies the loader
# (dist\tc-loader.dll -> game_engine.dll), and a sandbox that keeps an older
# loader silently tests the previous build.
& (Join-Path $PSScriptRoot 'make-ui-sandbox.ps1') -Sandbox $Sandbox -Mods $taskMod
"Loader under test: $((Get-FileHash (Join-Path $taskRoot 'game_engine.dll') -Algorithm SHA256).Hash)"
Remove-Item -LiteralPath $taskLog -ErrorAction SilentlyContinue

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskProcess = $null
$taskText = ''
$taskExitedDuringRun = $false
$taskStop = if ($Example) {
  'Game handle probe: armed|Native failed|Plugin callback threw'
} else {
  'DRIVER: done|DRIVER: giving up|DRIVER: the scene change did not|DRIVER: the second board never|Native failed|Plugin callback threw'
}
try {
  $env:USERPROFILE = Join-Path $Sandbox 'home'
  $env:APPDATA = Join-Path $Sandbox 'home\AppData\Roaming'
  # Hidden: the driver presses its own entry and changes the scene by itself, so
  # the game never takes over the desktop.
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = (Get-Date).AddSeconds($Seconds)
  do {
    Start-Sleep -Milliseconds 500
    $taskText = if (Test-Path -LiteralPath $taskLog) { Get-Content -LiteralPath $taskLog -Raw } else { '' }
    if ($taskText -match $taskStop -or $taskProcess.HasExited) { break }
  } while ((Get-Date) -lt $taskDeadline)
  # Recorded before this script asks the game to close: a process that is gone at
  # this point left on its own, which is a different failure from a timed-out flow.
  $taskExitedDuringRun = $taskProcess.HasExited
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id }
  }
}

# Distinguish "the flow timed out" from "the game left the building": a driver
# that presses its way through the game's own screens must not be able to hide a
# crash behind a missing log line.
if ($taskExitedDuringRun) {
  "Game process exited by itself during the run with code $($taskProcess.ExitCode)"
}

if (!(Test-Path -LiteralPath $taskLog)) { throw "Loader log missing: $taskLog" }
$taskLines = Get-Content -LiteralPath $taskLog
$taskText = $taskLines -join "`n"
$taskFatal = $taskLines | Where-Object {
  $_ -match 'Native failed|Plugin callback threw|回调异常'
}
if ($taskFatal) { $taskFatal | Select-Object -First 8; throw 'The probe or the loader reported a failure' }

if ($Example) {
  foreach ($taskExpected in @(
      'Game handle probe: armed (read-only)',
      'PROBE: Board unavailable on the main menu (unavailable)',
      'Native loaded: dev.game-handle-probe')) {
    if (!$taskText.Contains($taskExpected)) {
      $taskLines | Select-Object -Last 20
      throw "Missing evidence for the read-only probe: $taskExpected"
    }
  }
  ($taskLines | Where-Object { $_ -match 'Game handle probe|PROBE: Board|Native loaded' }) |
    Select-Object -First 4
  'PASS game handle probe: loads, declares game_handles, and the menu answers UNAVAILABLE'
  "Sandbox: $Sandbox"
  return
}

# The loader has to own scene.change before any plugin loads: that is what makes
# the invalidation below independent of what the installed mods hook.
if (!$taskText.Contains('Event source scene.change armed (Board handle tracking)')) {
  $taskLines | Select-Object -First 30
  throw 'The loader did not arm scene.change tracking before the plugin loaded'
}
# ... and the switch that follows the level load has to be read as *entering*
# the board scene: the pinned build loads the level first and switches the scene
# afterwards, so invalidating there would leave the whole level without a handle.
if (!($taskLines | Where-Object { $_ -match "Game handles: scene \d+ in the level's own frame; the Board handle stays valid" })) {
  $taskLines | Select-Object -First 30
  throw 'The loader did not keep the Board handle across the board-scene switch'
}
foreach ($taskExpected in @(
    'PROBE: level.load name=',
    'PROBE: previous handle valid=0 after the new level load',
    'PROBE: Board available generation=',
    'PROBE: component handle kind guard=kind',
    'PROBE: current after scene change=unavailable',
    'DRIVER: board A handle observed',
    'DRIVER: board B handle observed',
    "DRIVER: trying the game's own way out (Escape)",
    'DRIVER: self-exit=ok',
    'DRIVER: the game left the level by itself and the old handle was invalidated',
    'DRIVER: the game left the second level by itself and its handle was invalidated',
    'the old handle was invalidated',
    'DRIVER: done')) {
  if (!$taskText.Contains($taskExpected)) {
    $taskLines | Select-Object -Last 25
    throw "Missing evidence: $taskExpected"
  }
}

# The menu line has to come before the first level handle: the probe reported
# "no board" while the menu was up, not after a level had already loaded.
$taskMenuIndex = ($taskLines | Select-String -Pattern 'PROBE: Board unavailable on the main menu' |
  Select-Object -First 1).LineNumber
$taskFirstLevelIndex = ($taskLines | Select-String -Pattern 'PROBE: level.load name=' |
  Select-Object -First 1).LineNumber
if (!$taskMenuIndex -or !$taskFirstLevelIndex -or $taskFirstLevelIndex -lt $taskMenuIndex) {
  throw 'The probe never reported the empty main menu before the first level'
}

# Both levels, from the plugin's own lines: resolve() must land on the board the
# event carried, the handle must validate, and the two must not share a
# generation or a token.
# Both boards have to come from the campaign screen's own entries and both exits
# from the player's own key.  A fallback still exercises the loader's own path,
# but it is not the evidence this probe exists for, so it fails here.
$taskEntrySites = @()
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'DRIVER: pressed the level entry at rva=0x([0-9a-f]+)') {
    $taskEntrySites += $Matches[1]
  }
}
if ($taskEntrySites.Count -lt 2) {
  $taskLines | Select-Object -Last 25
  throw "Only $($taskEntrySites.Count) level entries were pressed; expected one per board"
}
if ($taskText -match 'entry=fallback') {
  throw 'The second level had to be loaded by name: a campaign entry did not load a level'
}
$taskSelfExits = @($taskLines | Where-Object { $_ -match 'DRIVER: self-exit=ok' }).Count
if ($taskSelfExits -lt 2) {
  $taskLines | Where-Object { $_ -match 'DRIVER: self-exit' } | Select-Object -First 4
  throw "The player's own exit ran $taskSelfExits time(s); expected one per level"
}

$taskHandles = @()
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'PROBE: level handle generation=(\d+) token=(\d+) size=(\d+) resolve=(\S+) resolve-match=(\d) valid=(-?\d+)') {
    $taskHandles += [pscustomobject]@{
      Generation = [uint64]$Matches[1]
      Token      = [uint64]$Matches[2]
      Size       = [int]$Matches[3]
      Resolve    = $Matches[4]
      Match      = [int]$Matches[5]
      Valid      = [int]$Matches[6]
    }
  }
}
if ($taskHandles.Count -lt 2) {
  $taskLines | Select-Object -Last 25
  throw "The probe issued $($taskHandles.Count) handles, expected one per level"
}
$taskFirst = $taskHandles[0]
$taskSecond = $taskHandles[$taskHandles.Count - 1]
foreach ($taskHandle in $taskHandles) {
  if ($taskHandle.Size -ne 24) { throw "A handle is $($taskHandle.Size) bytes, expected 24" }
  if ($taskHandle.Resolve -ne 'ok' -or $taskHandle.Match -ne 1) {
    throw 'A level handle did not resolve to the board the LEVEL_LOAD event carried'
  }
  if ($taskHandle.Valid -ne 1) { throw "A fresh level handle validated as $($taskHandle.Valid)" }
}
if ($taskFirst.Generation -eq $taskSecond.Generation -and $taskFirst.Token -eq $taskSecond.Token) {
  throw 'The second level reused the first level''s handle identity'
}

# The board-scene switch must have kept the handle, and the player's own exit
# must have refused it - by its own validation and by a resolve that has to
# answer STALE while clearing the output pointer.  "self-exit=ok" is asserted
# above on purpose: the fallback (the driver calling change_scene itself) still
# exercises the same loader path, but it is not the evidence this probe exists
# for, so a build where Escape stops leaving the level has to be investigated
# instead of quietly passing on the fallback.
if (!($taskLines | Where-Object { $_ -match 'PROBE: scene\.change scene=1 frame=\d+ old-handle-valid=1' })) {
  $taskLines | Where-Object { $_ -match 'PROBE: scene.change' } | Select-Object -First 5
  throw 'Entering the board scene invalidated the fresh handle'
}
if (!($taskLines | Where-Object { $_ -match 'PROBE: scene\.change scene=0 frame=\d+ old-handle-valid=0' })) {
  $taskLines | Where-Object { $_ -match 'PROBE: scene.change' } | Select-Object -First 5
  throw 'The scene-change event did not observe the old handle as invalid'
}
$taskStale = $taskLines | Where-Object { $_ -match 'PROBE: old handle resolve=stale' } | Select-Object -First 1
if (!$taskStale) {
  $taskLines | Select-Object -Last 20
  throw 'Resolving the old handle after the scene change did not answer STALE'
}
if (!($taskStale -match 'pointer=cleared')) {
  throw "A failed resolve left its output pointer set: $taskStale"
}
if (!($taskLines | Where-Object { $_ -match 'PROBE: Board unavailable again \(unavailable\)' })) {
  $taskLines | Select-Object -Last 20
  throw 'The Board query did not go back to UNAVAILABLE after leaving the level'
}

$taskHandles | Select-Object -First 4 | Format-Table -AutoSize | Out-String -Width 120 | Write-Host
($taskLines | Where-Object { $_ -match 'DRIVER: (board|the old handle|pressing)' }) | Select-Object -First 6
"Level A: generation=$($taskFirst.Generation) token=$($taskFirst.Token); level B: " +
"generation=$($taskSecond.Generation) token=$($taskSecond.Token)"
"PASS Board handles: the menu answered UNAVAILABLE, each level issued a handle that resolved to " +
"its own LEVEL_LOAD board and validated, the reserved COMPONENT kind was refused, leaving the " +
"level invalidated the old handle (validate 0, resolve STALE, query UNAVAILABLE), and the second " +
"level got a new generation and token"
"Sandbox: $Sandbox"
