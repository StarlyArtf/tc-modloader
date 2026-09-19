# Runs the UI example (mod-inspector) in an isolated sandbox and asserts that its
# panel actually drew a frame through sdk/tc_ui.h.
#
# The panel logs one line the first time it is drawn:
#   Mod Inspector: panel drawn; viewport=1920x1080 mouse=812,437 frame=1234
# A plausible viewport and mouse position prove the engine entry points behind
# tc::ui::viewportSize()/mousePos() were resolved and called with the right ABI;
# a wrong signature shows up here as nonsense values instead.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskSeconds = if($env:TC_SMOKE_SECONDS) { [int]$env:TC_SMOKE_SECONDS } else { 15 }
$taskMod = Join-Path $taskRepo 'dist\tcmod.mod-inspector.mod'
if(!(Test-Path -LiteralPath $taskMod)) { throw 'Missing dist\tcmod.mod-inspector.mod; run build.ps1 first' }

$taskTest = Join-Path $taskRepo ('build\ui-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods') | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath $taskMod -Destination (Join-Path $taskRoot 'mods\tcmod.mod-inspector.mod')
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply tcmod.mod-inspector
if($LASTEXITCODE){throw 'Inspector package apply failed'}

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
if(!(Test-Path -LiteralPath $taskLog)) { throw "Loader log missing: $taskLog" }
$taskLines = Get-Content -LiteralPath $taskLog
$taskLine = $taskLines | Where-Object { $_ -match 'Mod Inspector: panel drawn' } | Select-Object -Last 1
if(!$taskLine) {
  $taskLines | Select-Object -Last 15
  throw 'The inspector panel never drew a frame through tc_ui.h'
}
# Plugin log lines all carry a "[mod id]" prefix, so failures are detected by
# the loader's own wording rather than by the prefix.
if($taskLines | Where-Object { $_ -match 'Plugin callback threw|Native failed|回调异常' }) { throw 'The inspector callback reported an error' }
if($taskLine -notmatch 'viewport=(\d+)x(\d+) mouse=(-?\d+),(-?\d+)') { throw "Unparsable evidence line: $taskLine" }
$taskWidth = [int]$Matches[1]
$taskHeight = [int]$Matches[2]
if($taskWidth -lt 320 -or $taskHeight -lt 240) { throw "Implausible viewport in evidence line: $taskLine" }

$taskLine
if($taskWidth -lt 1024) { Write-Warning "Unexpectedly small viewport: $taskWidth x $taskHeight" }
"PASS UI panel rendered through tc_ui.h"
"Sandbox: $taskTest"
