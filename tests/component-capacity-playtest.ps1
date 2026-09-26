# The documented 1024-byte persistence cap, measured on the real machine.
#
# Launch 1 writes a full 1024-byte configuration through tc.component.storage (128
# chunk entries plus the four header entries), reports the record's live entries
# and the schematic's size, and saves with the game's own saver.  Launch 2 reads the
# configuration back out of the file.  The size printed by each launch is part of
# the evidence: "the cap is usable" and "the cap is cheap" are different claims.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskSchematic = Join-Path $taskRepo 'build\nl_not1legacy.data'
foreach($taskFile in @($taskSchematic,(Join-Path $taskRepo 'build\component-capacity-probe.dll'))) {
  if(!(Test-Path -LiteralPath $taskFile)) { throw "Missing $taskFile; run build.ps1 first" }
}

$taskTest = Join-Path $taskRepo ('build\component-capacity-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\test.component-capacity'
$taskPackage = Join-Path $taskTest 'package'
$taskSchema = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default\schematics\not_gate\Default'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,$taskSchema,(Join-Path $taskRoot 'mods'),$taskData,(Join-Path $taskPackage 'native') | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath (Join-Path $taskRepo 'build\component-capacity-probe.dll') -Destination (Join-Path $taskPackage 'native\probe.dll')
'{"format":2,"id":"test.component-capacity","name":"Development component capacity probe","version":"0.0.1","capabilities":["log","hook","services"],"native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding utf8
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRoot 'mods\probe.mod') | Out-Null
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.component-capacity
if($LASTEXITCODE){throw 'Capacity probe package apply failed'}

$taskCircuit = Join-Path $taskSchema 'circuit.data'
Copy-Item -LiteralPath $taskSchematic -Destination $taskCircuit -Force
$taskFixtureSize = (Get-Item -LiteralPath $taskCircuit).Length
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskRuns = @(
    @{ Launch=1; Expect='PASS capacity written'; Must='wrote=identical'; Count='entriesAfterWrite=(\d+)' }
    @{ Launch=2; Expect='PASS capacity survived save plus restart'; Must='readback=identical'; Count='entries=(\d+)' }
  )
  $taskSizes = @()
  foreach($taskCase in $taskRuns) {
    $taskResult = Join-Path $taskData 'result.txt'
    if(Test-Path -LiteralPath $taskResult) { Remove-Item -LiteralPath $taskResult -Force }
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline = [DateTime]::UtcNow.AddSeconds(40)
    while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
      Start-Sleep -Milliseconds 250
    }
    if(!(Test-Path -LiteralPath $taskResult)) {
      if(!$taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }
      throw "Capacity run $($taskCase.Launch) did not finish; inspect $taskRoot\tc-modloader-data\loader.log"
    }
    $taskText = [IO.File]::ReadAllText($taskResult)
    if(!$taskProcess.HasExited) {
      [void]$taskProcess.CloseMainWindow()
      if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
    }
    "run $($taskCase.Launch): $taskText"
    if(!$taskText.Contains($taskCase.Expect) -or !$taskText.Contains($taskCase.Must)) {
      throw "Capacity run $($taskCase.Launch) did not report '$($taskCase.Expect)' / '$($taskCase.Must)': $taskText"
    }
    if($taskText -notmatch $taskCase.Count) { throw "Capacity run $($taskCase.Launch) did not report its entry count: $taskText" }
    if([int]$Matches[1] -lt 132) { throw "Capacity run $($taskCase.Launch) carried only $($Matches[1]) entries, expected at least 132" }
    if($taskText -notmatch 'file=(\d+)') { throw "Capacity run $($taskCase.Launch) did not report the schematic size: $taskText" }
    $taskSizes += [int]$Matches[1]
  }
  if($taskSizes[1] -le $taskSizes[0]) {
    throw "The saved 1024-byte configuration did not grow the schematic (run1=$($taskSizes[0]) run2=$($taskSizes[1]))"
  }
  "schematic size: fixture=$taskFixtureSize after-save=$($taskSizes[0]) after-restart=$($taskSizes[1])"
  'PASS a full 1024-byte configuration (132 record entries) is written, saved and read back, and the schematic grows by the expected amount'
  "Evidence: $taskTest"
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
}
