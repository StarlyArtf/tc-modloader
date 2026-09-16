# Runs the built-in prototype dump probe in an isolated sandbox and prints it.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskSeconds = if($env:TC_SMOKE_SECONDS) { [int]$env:TC_SMOKE_SECONDS } else { 15 }
$taskMod = Join-Path $taskRepo 'dist\dev.kind-list.mod'
if(!(Test-Path -LiteralPath $taskMod)) { throw 'Missing dist\dev.kind-list.mod; run build.ps1 first' }

$taskTest = Join-Path $taskRepo ('build\kind-list-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.kind-list'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods'),$taskData | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath $taskMod -Destination (Join-Path $taskRoot 'mods\dev.kind-list.mod')
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.kind-list
if($LASTEXITCODE){throw 'Kind list package apply failed'}

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

$taskOut = Join-Path $taskRepo 'build\kinds.txt'
Copy-Item -LiteralPath (Join-Path $taskData 'kinds.txt') -Destination $taskOut -Force
Get-Content $taskOut | Select-Object -First 12
"... total " + ((Get-Content $taskOut).Count - 1) + " kinds"
"Saved: $taskOut"
"Sandbox: $taskTest"
