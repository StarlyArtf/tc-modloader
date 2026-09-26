# What a missing Mod actually costs, measured three ways.
#
# The fixture's level carries an instance of a custom component, and that
# instance already holds a saved configuration record (schema 6).  Three
# scenarios differ only in what happened before:
#
#   1. the profile never saw the Mod, and nothing is saved while it is missing:
#      the file keeps the record, so installing the Mod restores the configuration;
#   2. the same, but the player saves while the Mod is missing: the component is a
#      tombstone by then, so the record leaves the circuit file for good;
#   3. the Mod was installed once (the game learned the prototype) and then
#      removed: does the component survive on its own?
#
# Scenario 3 is the realistic "player disabled the Mod" case, and its answer
# decides how much the loader has to do about it.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskSchematic = Join-Path $taskRepo 'build\nl_not1legacy.data'
foreach($taskFile in @($taskSchematic,(Join-Path $taskRepo 'build\component-placeholder-probe.dll'),
                       (Join-Path $taskRepo 'build\component-storage-probe.dll'))) {
  if(!(Test-Path -LiteralPath $taskFile)) { throw "Missing $taskFile; run build.ps1 first" }
}

$taskTest = Join-Path $taskRepo ('build\component-placeholder-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\test.component-placeholder'
$taskStorageData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\test.component-storage'
$taskPackage = Join-Path $taskTest 'placeholder'
$taskStoragePackage = Join-Path $taskTest 'storage'
$taskSchema = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default\schematics\not_gate\Default'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,$taskSchema,(Join-Path $taskRoot 'mods'),$taskData,
  $taskStorageData,(Join-Path $taskPackage 'native'),(Join-Path $taskStoragePackage 'native') | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath (Join-Path $taskRepo 'build\component-placeholder-probe.dll') -Destination (Join-Path $taskPackage 'native\probe.dll')
Copy-Item -LiteralPath (Join-Path $taskRepo 'build\component-storage-probe.dll') -Destination (Join-Path $taskStoragePackage 'native\probe.dll')
'{"format":2,"id":"test.component-placeholder","name":"Development missing-Mod probe","version":"0.0.1","capabilities":["log","hook","services"],"native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding utf8
'{"format":2,"id":"test.component-storage","name":"Development component storage probe","version":"0.0.1","capabilities":["log","hook","services"],"native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content -LiteralPath (Join-Path $taskStoragePackage 'mod.json') -Encoding utf8

$taskCircuit = Join-Path $taskSchema 'circuit.data'
Copy-Item -LiteralPath $taskSchematic -Destination $taskCircuit -Force
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousMigrate = $env:TC_STORAGE_MIGRATE
# The probe's dump and the fixture's hash are what every scenario compares against.
$taskReport = Join-Path $taskData 'report.txt'
$taskFixtureHash = (Get-FileHash -LiteralPath $taskSchematic).Hash

function Invoke-Launch([string]$Label,[string]$ResultPath,[switch]$AllowFailure) {
  if(Test-Path -LiteralPath $ResultPath) { Remove-Item -LiteralPath $ResultPath -Force }
  $taskLogPath = Join-Path $taskRoot 'tc-modloader-data\loader.log'
  if(Test-Path -LiteralPath $taskLogPath) { Remove-Item -LiteralPath $taskLogPath -Force }
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds(40)
  while(!(Test-Path -LiteralPath $ResultPath) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
    Start-Sleep -Milliseconds 250
  }
  if(!(Test-Path -LiteralPath $ResultPath)) {
    if(!$taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }
    throw "$Label did not finish; inspect $taskRoot\tc-modloader-data\loader.log"
  }
  $taskText = [IO.File]::ReadAllText($ResultPath)
  if(!$AllowFailure -and !$taskText.StartsWith('PASS ')) { throw "$Label failed: $taskText" }
  if(!$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
  }
  # Keep each launch's log: the trace of prototype lookups is the evidence for
  # where a missing Mod's component is dropped.
  $taskLogCopy = Join-Path $taskTest (($Label -replace '[^A-Za-z0-9]+','-') + '.log')
  if(Test-Path -LiteralPath $taskLogPath) { Copy-Item -LiteralPath $taskLogPath -Destination $taskLogCopy -Force }
  return $taskText
}

function Get-LaunchLog([string]$Label) {
  return (Join-Path $taskTest (($Label -replace '[^A-Za-z0-9]+','-') + '.log'))
}
# The loader's own missing-Mod report: it must name the dropped component, its
# position and rotation, and it must not fire when the owner is installed.
function Assert-MissingModReported([string]$Label,[bool]$Expected) {
  $taskLog = Get-LaunchLog $Label
  $taskReported = Select-String -LiteralPath $taskLog -Pattern 'Missing Mod: custom 0x4e4f54315f303031 at \(-5,0\) rotation 0' -Quiet
  if($Expected -and !$taskReported) {
    throw "$Label did not report the dropped component; inspect $taskLog"
  }
  if(!$Expected -and $taskReported) {
    throw "$Label reported a missing Mod for a component that has an owner; inspect $taskLog"
  }
}

try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  Remove-Item Env:\TC_STORAGE_MIGRATE -ErrorAction SilentlyContinue

  # Phases 1-2: the probe is applied, but nothing registers the instance's custom
  # id, so the component is a real "missing Mod" for the game.
  & (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRoot 'mods\placeholder.mod') | Out-Null
  & (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskStoragePackage -Output (Join-Path $taskRoot 'mods\storage.mod') | Out-Null

  # --- scenario 0: the control --------------------------------------------------------
  # Both probes applied, so the component's owner IS registered.  This is the baseline
  # the missing-Mod dumps are compared against: same fixture, same dump code.
  Copy-Item -LiteralPath $taskSchematic -Destination $taskCircuit -Force
  & (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.component-placeholder test.component-storage
  if($LASTEXITCODE){throw 'Probe packages could not be enabled'}
  $env:TC_STORAGE_MIGRATE = 'accept'
  $taskControl = Invoke-Launch 'Scenario 0 (Mod installed)' (Join-Path $taskData 'result.txt')
  Remove-Item Env:\TC_STORAGE_MIGRATE -ErrorAction SilentlyContinue
  "scenario 0: $taskControl"
  if($taskControl -notmatch 'component=present') { throw "Scenario 0: the control did not see the component: $taskControl" }
  if($taskControl -notmatch 'custom=1') { throw "Scenario 0: the control lost the component: $taskControl" }
  $taskControlReport = [IO.File]::ReadAllText($taskReport)
  if($taskControlReport -notmatch 'custom=4e4f54315f303031') { throw "Scenario 0: the control lost the custom id: $taskControlReport" }
  if($taskControlReport -notmatch 'x=-5 y=0 rot=0') { throw "Scenario 0: the control lost the position: $taskControlReport" }
  Assert-MissingModReported 'Scenario 0 (Mod installed)' $false
  Copy-Item -LiteralPath $taskReport -Destination (Join-Path $taskTest 'report-0-control.txt') -Force

  & (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.component-placeholder
  if($LASTEXITCODE){throw 'Placeholder probe package apply failed'}

  # --- scenario 1: missing, not saved -------------------------------------------------
  $taskFirst = Invoke-Launch 'Scenario 1 (no Mod, no save)' (Join-Path $taskData 'result.txt')
  "scenario 1: $taskFirst"
  if($taskFirst -notmatch 'component=missing') { throw "Scenario 1 expected the component to be a tombstone: $taskFirst" }
  Assert-MissingModReported 'Scenario 1 (no Mod, no save)' $true
  if($taskFirst -notmatch 'tombstones=1') { throw "Scenario 1 did not see exactly one tombstone: $taskFirst" }
  if($taskFirst -notmatch 'wires=2') { throw "Scenario 1 lost the wires: $taskFirst" }
  if((Get-FileHash -LiteralPath $taskCircuit).Hash -ne $taskFixtureHash) {
    throw 'Scenario 1: merely loading the level without the Mod rewrote the schematic'
  }
  Copy-Item -LiteralPath $taskReport -Destination (Join-Path $taskTest 'report-1-missing.txt') -Force
  # The dangling anchors are the point: the wires still end at the pin positions the
  # component had, which is what a reinstall has to line up with again.
  $taskReportText = [IO.File]::ReadAllText($taskReport)
  if($taskReportText -notmatch 'wire=0 a=\(-6,-1\) b=\(-12,0\)') { throw "Scenario 1 moved a wire: $taskReportText" }
  if($taskReportText -notmatch 'wire=1 a=\(12,0\) b=\(-3,-1\)') { throw "Scenario 1 moved a wire: $taskReportText" }

  # --- scenario 2: install the Mod again after an unsaved load without it --------------
  Copy-Item -LiteralPath $taskSchematic -Destination $taskCircuit -Force
  & (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.component-storage
  if($LASTEXITCODE){throw 'Storage probe package apply failed'}
  $env:TC_STORAGE_MIGRATE = 'accept'
  $taskRestored = Invoke-Launch 'Scenario 2 (Mod reinstalled)' (Join-Path $taskStorageData 'result.txt')
  if($taskRestored -notmatch 'migrated value=a55a0ff0 migrations=1 legacy=1') {
    throw "Scenario 2: the reinstalled Mod did not get its stored configuration back: $taskRestored"
  }
  "scenario 2: $taskRestored"

  # --- scenario 3: the record is gone once a save happened without the Mod -------------
  # Take the Mod away again (only the reporting probe stays enabled) and give the level
  # a fresh copy of the fixture, so this measures the save without the Mod and nothing else.
  & (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.component-placeholder | Out-Null
  Copy-Item -LiteralPath $taskSchematic -Destination $taskCircuit -Force
  $env:TC_PLACEHOLDER_SAVE = '1'
  $taskSaved = Invoke-Launch 'Scenario 3 (no Mod, save)' (Join-Path $taskData 'result.txt')
  Remove-Item Env:\TC_PLACEHOLDER_SAVE -ErrorAction SilentlyContinue
  "scenario 3: $taskSaved"
  if($taskSaved -notmatch 'component=missing') { throw "Scenario 3 expected the component to be a tombstone: $taskSaved" }
  if($taskSaved -notmatch 'saved=1') { throw "Scenario 3 did not save: $taskSaved" }
  Assert-MissingModReported 'Scenario 3 (no Mod, save)' $true
  if(!(Select-String -LiteralPath (Get-LaunchLog 'Scenario 3 (no Mod, save)') -Pattern 'Missing Mod: kept 1 record\(s\) for level not_gate' -Quiet)) {
    throw 'Scenario 3 did not keep the dropped record for a later reinstall'
  }
  if((Get-FileHash -LiteralPath $taskCircuit).Hash -eq $taskFixtureHash) {
    throw 'Scenario 3: the save did not rewrite the schematic, so nothing was measured'
  }
  Copy-Item -LiteralPath $taskReport -Destination (Join-Path $taskTest 'report-3-saved.txt') -Force
  & (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.component-storage | Out-Null
  $taskGone = Invoke-Launch 'Scenario 3 (Mod back, after the saved loss)' (Join-Path $taskStorageData 'result.txt') -AllowFailure
  "scenario 3 (Mod back): $taskGone"
  # The rescue is what makes the loss survivable: the component comes back from the
  # loader's own store, with the configuration the capture kept.
  if($taskGone -notmatch 'migrated value=a55a0ff0 migrations=1 legacy=1') {
    throw "Scenario 3: the reinstalled Mod did not get the dropped component and its configuration back: $taskGone"
  }
  $taskRescueLog = Get-LaunchLog 'Scenario 3 (Mod back, after the saved loss)'
  if(!(Select-String -LiteralPath $taskRescueLog -Pattern 'Missing Mod rescue: put custom 0x4e4f54315f303031 back at \(-5,0\) rotation 0 with 5 stored configuration entries' -Quiet)) {
    throw "Scenario 3: the loader did not report the rescue; inspect $taskRescueLog"
  }
  if(!(Select-String -LiteralPath $taskRescueLog -Pattern 'Missing Mod: kept 0 record\(s\) for level not_gate' -Quiet)) {
    throw "Scenario 3: the rescued record was not cleared from the store; inspect $taskRescueLog"
  }

  # --- scenario 4: the profile knew the Mod once, then the player removed it ------------
  # This is the realistic "player disabled the Mod" case: the circuit file was written
  # while the Mod was installed, and the question is whether the game keeps a component
  # whose owner is no longer there.
  Copy-Item -LiteralPath $taskSchematic -Destination $taskCircuit -Force
  & (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.component-storage | Out-Null
  $env:TC_STORAGE_MIGRATE = 'accept'
  $taskKnown = Invoke-Launch 'Scenario 4 (Mod installed once)' (Join-Path $taskStorageData 'result.txt')
  "scenario 4 (Mod installed): $taskKnown"
  $taskAfterMod = (Get-FileHash -LiteralPath $taskCircuit).Hash
  & (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.component-placeholder | Out-Null
  Remove-Item Env:\TC_STORAGE_MIGRATE -ErrorAction SilentlyContinue
  $taskKnownAgain = Invoke-Launch 'Scenario 4 (Mod removed again)' (Join-Path $taskData 'result.txt')
  "scenario 4 (Mod removed): $taskKnownAgain"
  Assert-MissingModReported 'Scenario 4 (Mod removed again)' $true
  Copy-Item -LiteralPath $taskReport -Destination (Join-Path $taskTest 'report-4-known.txt') -Force
  if((Get-FileHash -LiteralPath $taskCircuit).Hash -ne $taskAfterMod) {
    throw 'Scenario 4: loading without the Mod rewrote the schematic the Mod had saved'
  }

  'PASS missing Mod: the component becomes a tombstone, the file survives until a save, a reinstall restores the configuration, and a save without the Mod is recovered from the loader''s own store once the Mod is back'
  "Evidence: $taskTest"
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  if($null -eq $taskPreviousMigrate) { Remove-Item Env:\TC_STORAGE_MIGRATE -ErrorAction SilentlyContinue }
  else { $env:TC_STORAGE_MIGRATE = $taskPreviousMigrate }
}
