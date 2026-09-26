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
      'Game handle probe: Board v3/v4, command/lifecycle/transaction v1 armed',
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

# The hook argument is a borrowed Nim string.  The event must expose its text
# during the callback instead of satisfying the broad prefix check with the old
# `(none)` placeholder.
$taskLevelNames = @($taskLines | Select-String -Pattern 'PROBE: level\.load name=([^ ]+)' |
  ForEach-Object { $_.Matches[0].Groups[1].Value })
if ($taskLevelNames.Count -lt 2 -or $taskLevelNames -contains '(none)') {
  $taskLines | Select-Object -Last 25
  throw "Level-load events did not carry both real level names: $($taskLevelNames -join ', ')"
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
$taskSnapshots = @($taskLines | Where-Object {
  $_ -match 'PROBE: snapshot status=0 version=1 frame=-?\d+ cycle=-?\d+ selected-components=\d+ selected-wires=\d+ flags=15'
})
if ($taskSnapshots.Count -lt 2) {
  $taskLines | Where-Object { $_ -match 'PROBE: snapshot' } | Select-Object -First 5
  throw "The probe captured $($taskSnapshots.Count) complete Board snapshots, expected one per level"
}
$taskObjectSnapshots = @($taskLines | Where-Object {
  $_ -match 'PROBE: objects status=0 components=(\d+) wires=(\d+) written=(\d+) resolved=(\d+) generation=(\d+)'
})
if ($taskObjectSnapshots.Count -lt 2) {
  $taskLines | Where-Object { $_ -match 'PROBE: objects' } | Select-Object -First 5
  throw "The probe captured $($taskObjectSnapshots.Count) complete object snapshots, expected one per level"
}
foreach($taskObjectLine in $taskObjectSnapshots){
  if($taskObjectLine -notmatch 'components=(\d+) wires=(\d+) written=(\d+) resolved=(\d+)'){throw "Bad object snapshot line: $taskObjectLine"}
  $taskExpectedChildren=[int64]$Matches[1]+[int64]$Matches[2]
  if([int64]$Matches[3] -ne $taskExpectedChildren -or [int64]$Matches[4] -ne $taskExpectedChildren){throw "Object handles were not all written and resolved: $taskObjectLine"}
}
if(!($taskLines | Where-Object { $_ -match 'PROBE: child handle next-frame valid=0' })){
  throw 'A Component/Wire handle survived beyond its snapshot frame'
}
$taskBoardV4=@($taskLines | Where-Object { $_ -match 'PROBE: board v4=4 prefix=1' })
if($taskBoardV4.Count -lt 1){throw 'Board V4 did not repeat the V3 service prefix'}
$taskBoardV5=@($taskLines | Where-Object { $_ -match 'PROBE: board v5=5 prefix=1' })
if($taskBoardV5.Count -lt 1){throw 'Board V5 did not repeat the V4 service prefix'}
$taskBoardV6=@($taskLines | Where-Object { $_ -match 'PROBE: board v6=6 prefix=1' })
if($taskBoardV6.Count -lt 1){throw 'Board V6 did not repeat the V5 service prefix'}
$taskComponents=@($taskLines | Where-Object {
  $_ -match 'PROBE: component info status=0 read=(\d+) of=(\d+) unique-ids=(\d+)'
})
if($taskComponents.Count -lt 2){
  $taskLines | Where-Object { $_ -match 'PROBE: component info' } | Select-Object -First 5
  throw "The probe read $($taskComponents.Count) complete component sets, expected one per level"
}
# Every object the enumeration issued has to come back with a real record, and
# the ids the game keeps on those records have to be unique within a level.
foreach($taskComponentLine in $taskComponents){
  if($taskComponentLine -notmatch 'read=(\d+) of=(\d+) unique-ids=(\d+) custom=(\d+) kind0=(\d+) xy0=(-?\d+),(-?\d+) rotation0=(\d+) id0=(\d+)'){
    throw "Bad component info line: $taskComponentLine"
  }
  if([int64]$Matches[1] -ne [int64]$Matches[2] -or [int64]$Matches[1] -lt 1){
    throw "Components were not all readable: $taskComponentLine"
  }
  if([int64]$Matches[3] -ne [int64]$Matches[1]){throw "Component ids were not unique: $taskComponentLine"}
  if([int64]$Matches[4] -gt [int64]$Matches[1]){throw "More custom components than components: $taskComponentLine"}
}
# Every board's component sequence carries one zero-filled entry at index 0
# (kind 0, which is not a kind the game defines).  It is not a component, so the
# probe asserts the histogram instead of pretending the first entry is one.
$taskComponentKinds=@($taskLines | Where-Object { $_ -match 'PROBE: component kinds ' })
if($taskComponentKinds.Count -lt 2){throw "The probe did not report component kind histograms"}
if(!(@($taskComponentKinds | Where-Object { $_ -notmatch 'PROBE: component kinds 0:1\s*$' })).Count){
  throw 'No level reported a real component kind, so the record fields were never decoded'
}
foreach($taskKindLine in $taskComponentKinds){
  if($taskKindLine -match 'PROBE: component kinds 0:([2-9]|\d\d)'){
    throw "More than one zero-filled placeholder entry: $taskKindLine"
  }
}
# Board V5 pins: the shapes asserted here are the pinned build's own entries for
# these two kinds (build/kinds.txt: 0x3c "Input" 0 in/1 out at (1,0) w1,
# 0x44 "Output" 1 in at (-1,0) w1).  A wrong prototype lookup or a wrong pin
# anchor cannot produce both lines.
$taskPinLines=@($taskLines | Where-Object { $_ -match 'PROBE: pins ' })
if($taskPinLines.Count -lt 2){
  $taskLines | Where-Object { $_ -match 'PROBE: pins|PROBE: component pins' } | Select-Object -First 5
  throw "The probe read $($taskPinLines.Count) component pin sets, expected one per real component"
}
foreach($taskExpectedPin in @(
    'PROBE: pins \d+ kind=0x3c in=0 out=1 p0=o\(1,0,w1\)',
    'PROBE: pins \d+ kind=0x44 in=1 out=0 p0=i\(-1,0,w1\)')){
  if(!($taskLines | Where-Object { $_ -match $taskExpectedPin })){
    $taskLines | Where-Object { $_ -match 'PROBE: pins' } | Select-Object -First 5
    throw "Missing component pin evidence: $taskExpectedPin"
  }
}
$taskPinSummary=@($taskLines | Where-Object {
  $_ -match 'PROBE: component pins status=\d+ read=(\d+) of=(\d+) pins=(\d+) expected=(\d+) zero=(\d+) auto=(\d+)'
})
if($taskPinSummary.Count -lt 2){
  $taskLines | Where-Object { $_ -match 'PROBE: component pins' } | Select-Object -First 5
  throw "The probe summarised $($taskPinSummary.Count) pin reads, expected one per level"
}
$taskPinRefusal=$false
$taskPinPlaceholder=$false
foreach($taskPinLine in $taskPinSummary){
  if($taskPinLine -notmatch 'read=(\d+) of=(\d+) pins=(\d+) expected=(\d+) zero=(\d+)'){
    throw "Bad pin summary line: $taskPinLine"
  }
  # Every component must resolve to a prototype and hand back exactly the pins
  # that prototype declares.
  if([int64]$Matches[1] -ne [int64]$Matches[2]){throw "A component had no prototype: $taskPinLine"}
  if([int64]$Matches[3] -ne [int64]$Matches[4]){throw "Pins were not all readable: $taskPinLine"}
  # kind 0 is a real (empty) entry in the game's own prototype table, so the
  # zero-filled placeholder resolves to zero pins rather than being invented
  # into a component.
  if([int64]$Matches[5] -ge 1){$taskPinPlaceholder=$true}
  if([int64]$Matches[3] -ge 1){$taskPinRefusal=$true}
}
if(!$taskPinPlaceholder -or !$taskPinRefusal){
  throw 'The V5 pin read did not show both an empty-prototype entry and a component with real pins'
}
$taskWires=@($taskLines | Where-Object {
  $_ -match 'PROBE: wire info status=0 read=(\d+) of=(\d+) endpoint=(\d+) width=(\d+) slot=(\d+)'
})
if($taskWires.Count -lt 2){
  $taskLines | Where-Object { $_ -match 'PROBE: wire info' } | Select-Object -First 5
  throw "The probe read $($taskWires.Count) complete wire sets, expected one per level"
}
# Wires are read by sequence index, and the width/state-slot fields have to be
# inside the ranges the pinned build produced when they were verified.
foreach($taskWireLine in $taskWires){
  if($taskWireLine -notmatch 'read=(\d+) of=(\d+) endpoint=(\d+) width=(\d+) slot=(\d+) xy0=(-?\d+),(-?\d+)->(-?\d+),(-?\d+) width0=(\d+) slot0=(\d+)'){
    throw "Bad wire info line: $taskWireLine"
  }
  if([int64]$Matches[1] -ne [int64]$Matches[2]){throw "Wires were not all readable: $taskWireLine"}
  if([int64]$Matches[3] -ne [int64]$Matches[1]){throw "Wire endpoints were not reported: $taskWireLine"}
  if([int64]$Matches[4] -ne [int64]$Matches[1]){throw "Wire widths were not reported: $taskWireLine"}
  if([int64]$Matches[5] -ne [int64]$Matches[1]){throw "Wire state slots were not reported: $taskWireLine"}
  if([int64]$Matches[1] -gt 0 -and ([int64]$Matches[10] -lt 1 -or [int64]$Matches[10] -gt 64)){
    throw "A wire width was outside 1..64: $taskWireLine"
  }
}
if(!($taskLines | Where-Object { $_ -match 'PROBE: child read next-frame component=-3 wire=-2' })){
  throw 'A V4 read accepted an object handle that outlived its frame'
}
# Board V6: the campaign level's wire must resolve to the two pins it visibly
# connects - the output pin component's input port at (9,0) and the input pin
# component's output port at (-9,0) (build/kinds.txt: 0x44 in0=(-1,0),
# 0x3c out0=(1,0), components at (10,0) and (-10,0)).
$taskWireEnds=@($taskLines | Where-Object {
  $_ -match 'PROBE: wire ends status=0 end0=\(9,0,dir0,pin0,kind=0x44\) end1=\(-9,0,dir1,pin0,kind=0x3c\)'
})
if($taskWireEnds.Count -lt 1){
  $taskLines | Where-Object { $_ -match 'PROBE: wire ends' } | Select-Object -First 5
  throw 'The wire ends did not resolve to the pins the geometry predicts'
}
# tc.simulation: the state read has to report the cycle and the state buffer, and
# the value read through a wire's own slot has to obey the mask rule.
$taskSimStates=@($taskLines | Where-Object {
  $_ -match 'PROBE: sim state status=0 cycle=-?\d+ frame=-?\d+ state-size=(\d+) flags=(\d+)'
})
if($taskSimStates.Count -lt 2){
  $taskLines | Where-Object { $_ -match 'PROBE: sim state' } | Select-Object -First 5
  throw "The probe read $($taskSimStates.Count) simulation states, expected one per level"
}
foreach($taskSimLine in $taskSimStates){
  if($taskSimLine -notmatch 'state-size=(\d+) flags=(\d+)'){throw "Bad simulation state line: $taskSimLine"}
  if([int64]$Matches[1] -lt 1){throw "The simulation state buffer was not reported: $taskSimLine"}
  # cycle | engine frame | state buffer
  if([int64]$Matches[2] -ne 7){throw "The simulation state flags are wrong: $taskSimLine"}
}
$taskSimValues=@($taskLines | Where-Object {
  $_ -match 'PROBE: sim value status=0 raw-status=0 slot=\d+ width=(\d+) value=(\d+) raw=(\d+) masked=(\d+) agree=1'
})
if($taskSimValues.Count -lt 1){
  $taskLines | Where-Object { $_ -match 'PROBE: sim value' } | Select-Object -First 5
  throw 'The probe never read a wire value through the simulation service'
}
foreach($taskSimValue in $taskSimValues){
  if($taskSimValue -notmatch 'width=(\d+) value=(\d+) raw=(\d+) masked=(\d+)'){throw "Bad simulation value line: $taskSimValue"}
  $taskWidth=[int64]$Matches[1];$taskValue=[int64]$Matches[2];$taskRaw=[int64]$Matches[3];$taskMasked=[int64]$Matches[4]
  if($taskWidth -lt 1 -or $taskWidth -gt 64){throw "A wire width outside 1..64 was probed: $taskSimValue"}
  $taskExpected=if($taskWidth -ge 64){$taskRaw}else{$taskRaw -band ((1L -shl $taskWidth) - 1)}
  if($taskValue -ne $taskExpected -or $taskMasked -ne $taskExpected){
    throw "The value read did not follow the mask rule: $taskSimValue"
  }
}
# The level.load moment is not the settled board; the V4 generator of this
# service has to keep working on a later frame, including the placeholder entry.
$taskSettled=@($taskLines | Where-Object {
  $_ -match 'PROBE: settled objects status=0 components=(\d+) wires=(\d+) read=(\d+) unique-ids=(\d+) empty=(\d+) kinds='
})
if($taskSettled.Count -lt 2){
  $taskLines | Where-Object { $_ -match 'PROBE: settled objects' } | Select-Object -First 5
  throw "The probe re-enumerated $($taskSettled.Count) settled boards, expected one per level"
}
foreach($taskSettledLine in $taskSettled){
  if($taskSettledLine -notmatch 'components=(\d+) wires=(\d+) read=(\d+) unique-ids=(\d+) empty=(\d+)'){
    throw "Bad settled board line: $taskSettledLine"
  }
  if([int64]$Matches[3] -ne [int64]$Matches[1]){throw "A settled board was not fully readable: $taskSettledLine"}
  if([int64]$Matches[4] -ne [int64]$Matches[1]){throw "A settled board had duplicate component ids: $taskSettledLine"}
  if([int64]$Matches[5] -gt 1){throw "A settled board had more than one placeholder entry: $taskSettledLine"}
}
$taskCommands=@($taskLines | Where-Object { $_ -match 'PROBE: command complete state=3 result=0 submitted=-?\d+ completed=-?\d+' })
if($taskCommands.Count -lt 2){
  $taskLines | Where-Object { $_ -match 'PROBE: command' } | Select-Object -First 8
  throw "The command bus completed $($taskCommands.Count) requests, expected one per level"
}
$taskEntered=@($taskLines | Where-Object { $_ -match 'PROBE: lifecycle kind=1 sequence=\d+ frame=-?\d+ board-valid=1' })
$taskLeft=@($taskLines | Where-Object { $_ -match 'PROBE: lifecycle kind=2 sequence=\d+ frame=-?\d+ board-valid=0' })
if($taskEntered.Count -lt 2 -or $taskLeft.Count -lt 2){
  $taskLines | Where-Object { $_ -match 'PROBE: lifecycle' } | Select-Object -First 10
  throw "Lifecycle service observed entered=$($taskEntered.Count), left=$($taskLeft.Count); expected two of each"
}
$taskTransactions=@($taskLines | Where-Object { $_ -match 'PROBE: transaction complete state=4 result=0 staged=2 completed=2' })
if($taskTransactions.Count -lt 2){
  $taskLines | Where-Object { $_ -match 'PROBE: transaction' } | Select-Object -First 8
  throw "The transaction service committed $($taskTransactions.Count) staged stop+save batches, expected two"
}
# The structure-change event: the probe changes the board once (a built-in AND
# through the command bus) after its own transaction finished, and the loader
# must raise OBJECTS_CHANGED exactly once for that edit.  Live counts come from
# the public read path and skip the zero-filled placeholder.
$taskEdits=@($taskLines | Where-Object {
  $_ -match 'PROBE: edit state=3 result=0 live-before=0 live-after=1'
})
if($taskEdits.Count -lt 1){
  $taskLines | Where-Object { $_ -match 'PROBE: edit' } | Select-Object -First 5
  throw 'The board edit through the command bus did not add exactly one live component'
}
$taskLifecycleCounts=@($taskLines | Where-Object {
  $_ -match 'PROBE: lifecycle objects-changed=1 selection-changed=0'
})
if($taskLifecycleCounts.Count -lt 1){
  $taskLines | Where-Object { $_ -match 'PROBE: lifecycle objects' } | Select-Object -First 5
  throw 'The board edit did not raise exactly one OBJECTS_CHANGED event'
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
"level got a new generation and token; Board V3 handles, command/lifecycle V1, and transactional stop+save batches all passed"
"Sandbox: $Sandbox"
