# Runs the native custom-OR example in an isolated game copy.  The example
# registers a component whose internal circuit is AND, then replaces its
# behavior with a C++ OR callback through TCCustomLogicRuntime.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskMod = Join-Path $taskRepo 'dist\example.custom-or.mod'
if(!(Test-Path -LiteralPath $taskMod)) { throw 'Missing dist\example.custom-or.mod; run build.ps1 first' }
$taskSolution = Join-Path $taskRepo 'build\and2_solution.data'
if(!(Test-Path -LiteralPath $taskSolution)) { throw "Missing $taskSolution; run build.ps1 first" }

$taskTest = Join-Path $taskRepo ('build\custom-or-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.custom-or'
$taskSchema = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default\schematics\and_gate\Default'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods'),$taskData,$taskSchema | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath $taskMod -Destination (Join-Path $taskRoot 'mods\example.custom-or.mod')
Copy-Item -LiteralPath $taskSolution -Destination (Join-Path $taskSchema 'circuit.data')
Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value '1' -Encoding ascii
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply example.custom-or
if($LASTEXITCODE){throw 'Custom OR package apply failed'}

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds 25
  if(!$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
}

$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
if(!(Select-String -LiteralPath $taskLog -Pattern 'custom-or: registered native OR2' -Quiet)) {
  throw "Custom OR example did not register; inspect $taskLog"
}
if(!(Select-String -LiteralPath $taskLog -Pattern 'custom-or: cycle=1 output=1 expected=0 fail' -Quiet)) {
  throw "Custom OR example did not produce OR behavior; inspect $taskLog"
}
'PASS native custom OR component produced OR behavior through C++ callback'
"Sandbox: $taskTest"
