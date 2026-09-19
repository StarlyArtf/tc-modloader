# Launches the sandbox game with the page-registration demo so it can be looked
# at and clicked by hand.  Nothing is simulated here: the window is on screen
# and normal input applies.
#
#   ./tests/ui-page-demo.ps1
#
# What to look at (the script prints this too):
#   1. main menu, top right - the loader's Mods button, then one entry per
#      registered page ("Menu demo", "Menu demo peer" with two packages);
#   2. click an entry - a full-screen page replaces the menu;
#   3. click Apply, watch the Apply= counter; hover it to see the colour change;
#      click Reset for a plain ImGui button next to it;
#   4. click the back button top-left - the main menu comes back;
#   5. open a page again, then start a level from the menu (Esc back to the
#      menu first if the page blocks it) and check the page does not survive
#      into the level.
#
# The game's own files under D:/p are never touched: this runs from a copy in
# build/ui-page-sandbox with its own USERPROFILE/APPDATA.
param(
  [switch]$Peer,
  [switch]$KeepLog
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskSandbox = Join-Path $taskRepo 'build\ui-page-sandbox'
$taskMods = if ($Peer) { @('dev.menu-demo','dev.menu-demo-peer') } else { @('dev.menu-demo') }
if (!(Test-Path -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll'))) {
  throw 'Missing dist\tc-loader.dll; run ./build.ps1 first'
}

& (Join-Path $PSScriptRoot 'make-ui-sandbox.ps1') -Sandbox $taskSandbox -Mods $taskMods | Out-Null
$taskGame = Join-Path $taskSandbox 'game'
$taskLog = Join-Path $taskGame 'tc-modloader-data\loader.log'
if (!$KeepLog) { Remove-Item -LiteralPath $taskLog -ErrorAction SilentlyContinue }

Write-Host ''
Write-Host '--- what to do ---' -ForegroundColor Cyan
Write-Host ' 1. main menu, top right: the Mods button, then one entry per page'
Write-Host ' 2. click a page entry - a full-screen page should replace the menu'
Write-Host ' 3. click Apply (game colours) and Reset (plain ImGui) - Apply= must go up'
Write-Host ' 4. click the back button at the top left - the menu should come back'
Write-Host ' 5. open a page, then leave it and start a level: the page must not'
Write-Host '    survive into the level'
Write-Host 'Press Alt+F4 (or close the window) when finished.'
Write-Host ('Log: ' + $taskLog)
Write-Host ''

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
try {
  $env:USERPROFILE = Join-Path $taskSandbox 'home'
  $env:APPDATA = Join-Path $taskSandbox 'home\AppData\Roaming'
  # Windowed and on screen on purpose: this run is meant to be looked at.
  $taskProcess = Start-Process -FilePath (Join-Path $taskGame 'Turing Complete.exe') `
    -WorkingDirectory $taskGame -PassThru
  Write-Host ('Game started (pid ' + $taskProcess.Id + ').')
  $taskProcess.WaitForExit()
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
}

if (Test-Path -LiteralPath $taskLog) {
  Write-Host ''
  Write-Host '--- loader log (last lines) ---' -ForegroundColor Cyan
  Get-Content -LiteralPath $taskLog | Select-Object -Last 25
}
