# Runs the example circuit component mod in an isolated sandbox and reports
# what it registered, so the numbers the plugin writes can be checked without
# starting the player's own installation.  Optional env override:
#   TC_DESIGN_STATS="<gates> <delay>" writes that diagnostic override file.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskSeconds = if($env:TC_SMOKE_SECONDS) { [int]$env:TC_SMOKE_SECONDS } else { 20 }
$taskMod = Join-Path $taskRepo 'dist\example.circuit-and.mod'
if(!(Test-Path -LiteralPath $taskMod)) { throw 'Missing dist\example.circuit-and.mod; run build.ps1 first' }

$taskTest = Join-Path $taskRepo ('build\circuit-and-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.circuit-and'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods'),$taskData | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath $taskMod -Destination (Join-Path $taskRoot 'mods\example.circuit-and.mod')
if($env:TC_DESIGN_STATS) {
  Set-Content -LiteralPath (Join-Path $taskData 'design-stats.txt') -Value $env:TC_DESIGN_STATS -Encoding ascii
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply example.circuit-and
if($LASTEXITCODE){throw 'Example package apply failed'}

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds $taskSeconds
  if(!$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
}

$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskLines = Select-String -Path $taskLog -Pattern 'circuit-and:|example.circuit-and'
$taskLines | Select-Object -Last 8
if(!($taskLines | Where-Object { $_.Line -match 'registered AND2 Test' })) {
  throw "Example mod did not register its component; inspect $taskLog"
}
"Sandbox: $taskTest"
