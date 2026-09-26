# Runs tests/float-pitch-probe.cpp in a real game beside the Float Ops Mod and
# prints how close two components may be placed.
#
# The player's report: "这些元件在垂直方向上 footprint 好像有点大了，比外观要大
# 一点，导致现在这些元件相互之间不能贴在一起，而是中间有间隔."  The probe places a
# reference part and a second one `dy` cells below it through the game's own
# placement command, in its own column per trial, for this Mod's parts and for a
# stock part of the game's own library.  `pitch.txt` carries the table.
param(
  [int]$Seconds = 90,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskCxx = Join-Path $taskCompiler 'g++.exe'
$taskId = 'test.float-pitch-probe'
$taskLevel = 'architecture'
$taskMods = @('dev.enter-board', 'local.float-ops', $taskId)
$taskTest = Join-Path $taskRepo ('build\float-pitch-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskProfileDir = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
$taskData = Join-Path $taskRoot ('tc-modloader-data\plugin-data\' + $taskId)
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskFixture = Join-Path $taskRepo 'build\float-m2-board.data'
New-Item -ItemType Directory -Force $taskRoot, $taskProfile, (Join-Path $taskRoot 'mods'), $taskData,
  (Join-Path $taskProfileDir ('schematics\' + $taskLevel + '\Default')) | Out-Null
if (Test-Path -LiteralPath $taskFixture) {
  Copy-Item -LiteralPath $taskFixture -Destination `
    (Join-Path $taskProfileDir ('schematics\' + $taskLevel + '\Default\circuit.data')) -Force
}
foreach ($taskDir in @('asset', 'campaign', 'translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll', 'soft_oal.dll',
                         'steam_api64.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') `
  -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
foreach ($taskMod in @('dev.enter-board', 'local.float-ops')) {
  Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\' + $taskMod + '.mod')) `
    -Destination (Join-Path $taskRoot 'mods') -Force
}
@(
  'setting_window_mode = true'
  'setting_window_position = 0'
  'setting_window_size = 52430000'
  'setting_language = Chinese (Simplified)'
  ('setting_current_level = ' + $taskLevel)
) | Set-Content -LiteralPath (Join-Path $taskProfileDir 'settings.txt') -Encoding ascii

$taskPackage = Join-Path $taskTest 'probe-package'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native') | Out-Null
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static -shared -I (Join-Path $taskRepo 'sdk') `
  (Join-Path $PSScriptRoot 'float-pitch-probe.cpp') -o (Join-Path $taskPackage 'native\probe.dll')
if ($LASTEXITCODE) { throw 'The pitch probe did not compile' }
("{`"format`":2,`"id`":`"$taskId`",`"name`":`"Float pitch probe`",`"version`":`"0.1.0`"," +
 "`"native`":{`"api`":1,`"entry`":`"native/probe.dll`"}}") |
  Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding ascii
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage `
  -Output (Join-Path $taskRoot ('mods\' + $taskId + '.mod')) | Out-Null
if ($LASTEXITCODE) { throw 'The probe package was not written' }

& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskMods | Out-Host
if ($LASTEXITCODE) { throw 'The sandbox Mod apply failed' }

$taskProcess = $null
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousPalette = $env:TC_FLOATOPS_PALETTE
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $env:TC_FLOATOPS_PALETTE = $null
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  while ([DateTime]::UtcNow -lt $taskDeadline) {
    Start-Sleep -Milliseconds 500
    if ($taskProcess.HasExited) { break }
    if (Test-Path -LiteralPath (Join-Path $taskData 'pitch.txt')) { break }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:TC_FLOATOPS_PALETTE = $taskPreviousPalette
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
}

$taskReport = Join-Path $taskData 'pitch.txt'
if (!(Test-Path -LiteralPath $taskReport)) {
  throw "The probe wrote no pitch.txt (the game never opened a board); inspect $taskLog"
}
Copy-Item -LiteralPath $taskReport -Destination (Join-Path $taskRepo 'build\pitch.txt') -Force
Get-Content -LiteralPath $taskReport

# The numbers are the contract, because they are what the player feels:
#
#   * every face that fits inside the stock 4.92 x 2.93-cell look reserves three
#     cells vertically, so two of them sit flush.  The player's report was that
#     the multi-pin types reserved four or five ("这四个还是有点问题"), left over
#     from a face grown by a fixed margin and a box rounded outwards;
#   * Compare is the one type whose four-output side needs four cells of its own
#     (its face runs -0.33..3.33, i.e. the cells 0..3);
#   * horizontally the pitch is the face's five cells plus a cell for every pin
#     that reaches outside the box, so a two-sided type costs seven and a lone
#     output six - the game reserves a cell for the pin itself.
$taskPitchFile = Join-Path $taskRepo 'build\pitch.txt'
$taskText = (Get-Content -LiteralPath $taskPitchFile) -join "`n"
function Assert-Pitch([string]$Needle, [string]$Why) {
  if (!$taskText.Contains($Needle)) {
    throw "Missing measurement: $Needle ($Why); inspect $taskPitchFile"
  }
}
foreach ($taskFlush in @('subtract', 'constant', 'fma', 'split bits', 'make bits')) {
  Assert-Pitch ("measure float-ops $taskFlush vertical pitch=3") `
    'a stock-sized face must sit flush at three cells'
}
Assert-Pitch 'measure float-ops compare vertical pitch=4' `
  'the four-output side spans rows 0..3, which is four cells'
Assert-Pitch 'measure float-ops subtract horizontal pitch=7' `
  'five face cells plus one cell per pin outside the box'
Assert-Pitch 'measure float-ops constant horizontal pitch=6' `
  'the constant has no input pin to clear'
if ($taskText -match 'pitch>10') { throw "A placement pitch ran past ten cells; inspect build\pitch.txt" }
"PASS float-ops pitch: every stock-sized face reserves three cells vertically (Compare four, for its four-output side) and five plus a cell per outside pin horizontally, so the float parts sit flush like the game's own"
"dump: $(Join-Path $taskRepo 'build\pitch.txt')"
"sandbox: $taskTest"
