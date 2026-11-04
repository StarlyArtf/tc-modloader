# True-game case for docs/research/level-interface.md: which part of a level
# comes from disk?
#
# A Mod package may deploy files under campaign/ (the loader's journal allows
# that prefix), so the question is whether replacing a level's folder really
# replaces what the player sees: its board, its title, its dialogue.
#
# The case rewrites campaign/sandbox/ in an isolated copy of the game - a title
# and a dialogue line that appear nowhere in the shipped build, plus a board from
# tests/float-fixture.cpp (two constants, an adder, a display) - enters that
# level the way a player does (dev.enter-board) and lets the probe photograph the
# frame.  The picture is the evidence.
param(
  [int]$Seconds = 120,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskCxx = Join-Path $taskCompiler 'g++.exe'
$taskProbeId = 'test.level-content-probe'
$taskLevel = 'sandbox'
foreach ($taskRequired in @($taskCxx, (Join-Path $taskRepo 'dist\tc-loader.dll'),
                             (Join-Path $taskRepo 'dist\tcmod-cli.exe'),
                             (Join-Path $taskGame 'Turing Complete.exe'))) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}

# The board the level starts with: the M2 fixture (two FP32 constants -> adder ->
# display), so the picture shows parts the stock sandbox never has.
$taskFixtureExe = Join-Path $taskRepo 'build\float-fixture.exe'
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static (Join-Path $PSScriptRoot 'float-fixture.cpp') -o $taskFixtureExe
if ($LASTEXITCODE) { throw 'The fixture generator did not compile' }
$taskBoard = Join-Path $taskRepo 'build\float-m2-board.data'
& $taskFixtureExe --m2 $taskBoard | Out-Host
if ($LASTEXITCODE -or !(Test-Path -LiteralPath $taskBoard)) { throw 'The M2 fixture was not generated' }

$taskSandbox = Join-Path $taskRepo ('build\level-content-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskSandbox 'game'
$taskProfile = Join-Path $taskSandbox 'home'
$taskProfileDir = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskData = Join-Path $taskRoot ('tc-modloader-data\plugin-data\' + $taskProbeId)
New-Item -ItemType Directory -Force $taskRoot, $taskProfile,
  (Join-Path $taskRoot 'mods'), $taskData,
  (Join-Path $taskProfileDir ('schematics\' + $taskLevel + '\Default')) | Out-Null
foreach ($taskDir in @('asset', 'campaign', 'translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll', 'soft_oal.dll',
                         'steam_api64.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll') -Force

# ---- the rewritten level folder -------------------------------------------- #
$taskLevelDir = Join-Path $taskRoot ('campaign\' + $taskLevel)
if (!(Test-Path -LiteralPath $taskLevelDir)) { throw "The game has no campaign/$taskLevel" }
# The text ids are *fresh*, and that is the point: the game looks a level's text
# up in translations/<language>.txt by id, so an id that already exists shows the
# shipped translation instead of the text in meta.txt (measured: the first run of
# this case reused always_on's dialogue id and the popup came up translated to
# Chinese).  A Mod that ships a new level must use ids nobody has translated - or
# ship translations beside them.
$taskMeta = @(
  'kind = architecture'
  'background = 4'
  'size = 255'
  'title = (99999_10000000000001, `MODDED LEVEL TITLE`)'
  'dialogue = ['
  '  (mentor_centered, (99999_10000000000002, `this dialogue came from campaign/sandbox/meta.txt`))'
  ']'
) -join "`n"
# No BOM: the game parses this file itself, and the shipped levels have none.
[IO.File]::WriteAllText((Join-Path $taskLevelDir 'meta.txt'), $taskMeta,
                        (New-Object Text.UTF8Encoding($false)))
Copy-Item -LiteralPath $taskBoard -Destination (Join-Path $taskLevelDir 'circuit.data') -Force

foreach ($taskMod in @('dev.enter-board', 'local.float-ops')) {
  Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\' + $taskMod + '.mod')) -Destination (Join-Path $taskRoot 'mods') -Force
}
@(
  'setting_window_mode = true'
  'setting_window_position = 0'
  'setting_window_size = 52430000'
  'setting_language = Chinese (Simplified)'
  ('setting_current_level = ' + $taskLevel)
) | Set-Content -LiteralPath (Join-Path $taskProfileDir 'settings.txt') -Encoding ascii

# The probe: built here so it always matches the source next to it.
$taskProbePackage = Join-Path $taskSandbox 'probe-package'
New-Item -ItemType Directory -Force (Join-Path $taskProbePackage 'native') | Out-Null
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static -shared -I (Join-Path $taskRepo 'sdk') (Join-Path $PSScriptRoot 'level-content-probe.cpp') -o (Join-Path $taskProbePackage 'native\probe.dll')
if ($LASTEXITCODE) { throw 'The level content probe did not compile' }
("{`"format`":2,`"id`":`"$taskProbeId`",`"name`":`"Level content probe`",`"version`":`"0.1.0`"," +
 "`"capabilities`":[`"log`",`"status`",`"services`",`"events`"]," +
 "`"native`":{`"api`":1,`"entry`":`"native/probe.dll`"}}") | Set-Content -LiteralPath (Join-Path $taskProbePackage 'mod.json') -Encoding ascii
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskProbePackage -Output (Join-Path $taskRoot ('mods\' + $taskProbeId + '.mod')) | Out-Null
if ($LASTEXITCODE) { throw 'The probe package was not written' }

$taskApplied = @('dev.enter-board', 'local.float-ops', $taskProbeId)
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskApplied | Out-Host
if ($LASTEXITCODE) { throw 'The sandbox Mod apply failed' }

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskProcess = $null
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  while ([DateTime]::UtcNow -lt $taskDeadline) {
    Start-Sleep -Milliseconds 500
    if ($taskProcess.HasExited) { break }
    # The second picture is the one with the board: the probe clicks the intro
    # dialogue out of the way first.
    if (Test-Path -LiteralPath (Join-Path $taskData 'level2.bmp')) { break }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
}

if (!(Test-Path -LiteralPath $taskLog)) { throw "No loader log; inspect $taskSandbox" }
$taskLines = Get-Content -LiteralPath $taskLog -Encoding UTF8
$taskText = $taskLines -join "`n"
@($taskLines | Where-Object { $_ -match 'PROBE:|level\.load' }) | Select-Object -Last 10 | ForEach-Object { $_ }
if (!$taskText.Contains(('PROBE: level.load name="' + $taskLevel + '"'))) {
  throw "the level $taskLevel was not entered (inspect $taskLog)"
}
$taskCapture = Join-Path $taskData 'level.bmp'
if (!(Test-Path -LiteralPath $taskCapture)) { throw "the probe captured no frame; inspect $taskLog" }
Copy-Item -LiteralPath $taskCapture -Destination (Join-Path $taskRepo 'build\level-content.bmp') -Force
$taskCapture2 = Join-Path $taskData 'level2.bmp'
if (!(Test-Path -LiteralPath $taskCapture2)) { throw "the probe captured no board frame; inspect $taskLog" }
Copy-Item -LiteralPath $taskCapture2 -Destination (Join-Path $taskRepo 'build\level-content-board.bmp') -Force

"PASS level content: the game entered campaign/$taskLevel with a meta.txt and a circuit.data written by the case, and the probe photographed the frame"
"picture: $(Join-Path $taskRepo 'build\level-content.bmp')"
"sandbox: $taskSandbox"
