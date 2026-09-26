# One Ctrl+Z reverts one configuration commit.
#
# The probe writes two configurations through tc.component.storage, presses undo
# and redo through the game's own entry (the command bus reaches the same function
# the player's Ctrl+Z does), and reads the *board record* after each press - not the
# service, which keeps an in-memory copy that would hide a board-level mistake.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskSchematic = Join-Path $taskRepo 'build\nl_not1legacy.data'
foreach($taskFile in @($taskSchematic,(Join-Path $taskRepo 'build\component-undo-probe.dll'))) {
  if(!(Test-Path -LiteralPath $taskFile)) { throw "Missing $taskFile; run build.ps1 first" }
}

$taskTest = Join-Path $taskRepo ('build\component-undo-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\test.component-undo'
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
Copy-Item -LiteralPath (Join-Path $taskRepo 'build\component-undo-probe.dll') -Destination (Join-Path $taskPackage 'native\probe.dll')
'{"format":2,"id":"test.component-undo","name":"Development component undo probe","version":"0.0.1","capabilities":["log","hook","services"],"native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding utf8
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRoot 'mods\probe.mod') | Out-Null
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.component-undo
if($LASTEXITCODE){throw 'Component undo probe package apply failed'}

Copy-Item -LiteralPath $taskSchematic -Destination (Join-Path $taskSchema 'circuit.data') -Force
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskResult = Join-Path $taskData 'result.txt'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds(40)
  while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
    Start-Sleep -Milliseconds 250
  }
  if(!(Test-Path -LiteralPath $taskResult)) {
    if(!$taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }
    throw "Undo probe did not finish; inspect $taskRoot\tc-modloader-data\loader.log"
  }
  $taskText = [IO.File]::ReadAllText($taskResult)
  if(!$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
  }
  "undo probe: $taskText"
  if(!$taskText.StartsWith('PASS ')) { throw "The undo probe failed: $taskText" }
  'PASS one Ctrl+Z reverted one configuration commit and one Ctrl+Y re-applied it'
  "Evidence: $taskTest"
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
}
