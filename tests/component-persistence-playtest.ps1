param([ValidateSet('inplace','insert')][string]$Mode = 'inplace')
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskFixture = Join-Path $taskRepo 'build\and2_component.data'
# The inplace route needs the fixture whose custom tail already carries the
# sentinel pair; the insert route starts from the plain solution, whose tail is
# empty, and lets the game's own Table setter allocate and grow the table.
$taskSolution = Join-Path $taskRepo $(if($Mode -eq 'insert') {
  'build\and2_solution.data'
} else {
  'build\and2_solution_storage.data'
})
# 0x013579BDF2468ACE / 0x02468ACE13579BDF, the same pair the fixture uses.
$taskFirstValue = [uint64]0x13579BDF2468ACE
$taskSecondValue = [uint64]0x2468ACE13579BDF
foreach($taskFile in @($taskFixture,$taskSolution)) {
  if(!(Test-Path -LiteralPath $taskFile)) { throw "Missing $taskFile; run build.ps1 first" }
}

$taskTest = Join-Path $taskRepo ('build\component-persistence-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\test.component-persistence'
$taskPackage = Join-Path $taskTest 'package'
$taskSchema = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default\schematics\and_gate\Default'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,$taskSchema,(Join-Path $taskRoot 'mods'),(Join-Path $taskData 'fixtures'),(Join-Path $taskPackage 'native') | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath $taskFixture -Destination (Join-Path $taskData 'fixtures\and2_component.data')
Copy-Item -LiteralPath (Join-Path $taskRepo 'build\component-persistence-probe.dll') -Destination (Join-Path $taskPackage 'native\probe.dll')
'{"format":2,"id":"test.component-persistence","name":"Development component persistence probe","version":"0.0.1","native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding utf8
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRoot 'mods\probe.mod') | Out-Null
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.component-persistence
if($LASTEXITCODE){throw 'Component persistence probe package apply failed'}

$taskCircuit = Join-Path $taskSchema 'circuit.data'
Copy-Item -LiteralPath $taskSolution -Destination $taskCircuit -Force
$taskInitialHash = (Get-FileHash -LiteralPath $taskCircuit).Hash
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousMode = $env:TC_PERSISTENCE_MODE
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $env:TC_PERSISTENCE_MODE = $Mode
  foreach($taskRun in $(if($Mode -eq 'insert') { @(1,2,3) } else { @(1,2) })) {
    $taskResult = Join-Path $taskData 'result.txt'
    if(Test-Path -LiteralPath $taskResult) { Remove-Item -LiteralPath $taskResult -Force }
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline = [DateTime]::UtcNow.AddSeconds(40)
    while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
      Start-Sleep -Milliseconds 250
    }
    if(!(Test-Path -LiteralPath $taskResult)) {
      if(!$taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }
      throw "Persistence run $taskRun did not finish; inspect $taskRoot\tc-modloader-data\loader.log"
    }
    $taskText = [IO.File]::ReadAllText($taskResult)
    if(!$taskText.StartsWith('PASS ')) { throw "Persistence run $taskRun failed: $taskText" }
    if($Mode -eq 'insert') {
      # Run 1 must have inserted into the fixture's empty table and grown it;
      # runs 2 and 3 assert the exact value the game deserialized from the file.
      $taskExpect = switch($taskRun) {
        1 { "PASS insert inserted value=$taskFirstValue" }
        2 { "readback $taskFirstValue then updated value=$taskSecondValue" }
        3 { "readback $taskSecondValue stable value=$taskSecondValue" }
      }
      if(!$taskText.Contains($taskExpect)) {
        throw "Persistence run $taskRun asserted '$taskExpect' but reported: $taskText"
      }
    }
    if(!$taskProcess.HasExited) {
      [void]$taskProcess.CloseMainWindow()
      if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
    }
    "run $taskRun`: $taskText"
  }
  if((Get-FileHash -LiteralPath $taskCircuit).Hash -eq $taskInitialHash) {
    throw 'The first launch did not persist the changed custom-component tail value'
  }
  "PASS custom component tail value survives save plus restart (mode=$Mode)"
  "Evidence: $taskTest"
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:TC_PERSISTENCE_MODE = $taskPreviousMode
}
