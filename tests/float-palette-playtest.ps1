# True-game case: the float types live in a folder the *game* builds in its own
# component column.
#
# What the player asked for: "把浮点数mod的这些元件在游戏内右侧元件栏单独开一个
# 文件夹，也就是浮点文件夹，里面存放这些元件，而不是都放到自定义元件文件夹" and
# then "我希望分类文件夹是真的插进游戏自己的分类体系".  Measured
# (docs/research/palette-categories.md): that column is drawn from the game's own
# menu tree, and the game builds that tree out of a custom prototype's *name* -
# `add_to_menu_tree__presenterZutilities_u12208` splits the name on "/" and walks
# the parts as parent categories.  So the Mod names its types "浮点/FP32 Add",
# and the game itself files them under a 浮点 folder.  The Mod draws no palette
# of its own: no board side panel, no popup, nothing outside the game's own
# column.
#
# How the case proves it: `tests/float-menu-probe.cpp` hooks the game's own
# `reload_component_menu__presenterZutilities_u14192`, waits for it to run (the
# game called it to build the menu) and dumps the tree the palette will draw -
# `menu.txt`, one line per node, with the node's variant tag, name and custom id.
# The assertions below are about that dump, not about anything the Mod reports:
# a 浮点 category node has to exist inside the game's 自定义 category, and the
# Mod's own type ids have to be its children and nothing else.
param(
  [int]$Seconds = 60,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskCxx = Join-Path $taskCompiler 'g++.exe'
$taskId = 'test.float-menu-probe'
$taskLevel = 'architecture'
$taskMods = @('dev.enter-board', 'local.float-ops', $taskId)
foreach ($taskRequired in @($taskCxx, (Join-Path $taskRepo 'dist\tc-loader.dll'),
                             (Join-Path $taskRepo 'dist\tcmod-cli.exe'),
                             (Join-Path $taskGame 'Turing Complete.exe'))) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}

# The Mod under test is the package the player installs.
& (Join-Path $taskRepo 'examples\float-ops\build.ps1') -SkipTests | Out-Host
if ($LASTEXITCODE) { throw 'Float Ops build failed' }

$taskTest = Join-Path $taskRepo ('build\float-palette-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskProfileDir = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
$taskData = Join-Path $taskRoot ('tc-modloader-data\plugin-data\' + $taskId)
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
New-Item -ItemType Directory -Force $taskRoot, $taskProfile, (Join-Path $taskRoot 'mods'), $taskData,
  (Join-Path $taskProfileDir ('schematics\' + $taskLevel + '\Default')) | Out-Null
foreach ($taskDir in @('asset', 'campaign', 'translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll', 'soft_oal.dll',
                         'steam_api64.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
$taskFixture = Join-Path $taskRepo 'build\float-m2-board.data'
if (Test-Path -LiteralPath $taskFixture) {
  Copy-Item -LiteralPath $taskFixture -Destination `
    (Join-Path $taskProfileDir ('schematics\' + $taskLevel + '\Default\circuit.data')) -Force
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

# The probe, built here so it always matches the source next to it.
$taskPackage = Join-Path $taskTest 'probe-package'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native') | Out-Null
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static -shared -I (Join-Path $taskRepo 'sdk') `
  (Join-Path $PSScriptRoot 'float-menu-probe.cpp') -o (Join-Path $taskPackage 'native\probe.dll')
if ($LASTEXITCODE) { throw 'The menu probe did not compile' }
("{`"format`":2,`"id`":`"$taskId`",`"name`":`"Float menu probe`",`"version`":`"0.1.0`"," +
 "`"native`":{`"api`":1,`"entry`":`"native/probe.dll`"}}") |
  Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding ascii
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage `
  -Output (Join-Path $taskRoot ('mods\' + $taskId + '.mod')) | Out-Null
if ($LASTEXITCODE) { throw 'The probe package was not written' }

& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskMods | Out-Host
if ($LASTEXITCODE) { throw 'The sandbox Mod apply failed' }

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousPalette = $env:TC_FLOATOPS_PALETTE
$taskProcess = $null
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
    if (Test-Path -LiteralPath (Join-Path $taskData 'menu.txt')) { break }
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

$taskReport = Join-Path $taskData 'menu.txt'
if (!(Test-Path -LiteralPath $taskReport)) {
  $taskTail = if (Test-Path -LiteralPath $taskLog) {
    (Get-Content -LiteralPath $taskLog | Where-Object { $_ -match 'menu-probe|ENTER-BOARD' } |
      Select-Object -Last 20) -join "`n"
  } else { '' }
  throw "The probe wrote no menu.txt (the game never built its menu).`n$taskTail"
}
Copy-Item -LiteralPath $taskReport -Destination (Join-Path $taskRepo 'build\menu.txt') -Force
$taskLines = Get-Content -LiteralPath $taskReport

function Assert-TaskContains([string]$Needle) {
  if (!(($taskLines -join "`n").Contains($Needle))) {
    throw "Missing evidence: $Needle (inspect $taskReport)"
  }
}

# 1. The game's own tree still has its five categories, 自定义 among them.
Assert-TaskContains 'tag=2 name="布尔"'
Assert-TaskContains 'tag=2 name="自定义"'

# 2. Inside 自定义 the game built a 浮点 folder.  The dump indents a child by two
#    spaces, so the folder is the entry that is indented once under 自定义.
$taskCustomIndex = -1
for ($taskIndex = 0; $taskIndex -lt $taskLines.Count; ++$taskIndex) {
  if ($taskLines[$taskIndex] -match '^node=.*tag=2 name="自定义"') { $taskCustomIndex = $taskIndex }
}
if ($taskCustomIndex -lt 0) { throw 'the game tree has no top-level 自定义 node' }
$taskFolderIndex = -1
for ($taskIndex = $taskCustomIndex + 1; $taskIndex -lt $taskLines.Count; ++$taskIndex) {
  $taskLine = $taskLines[$taskIndex]
  if ($taskLine -match '^node=') { break }
  if ($taskLine -match '^\s\snode=.*tag=2 name="浮点"') { $taskFolderIndex = $taskIndex; break }
}
if ($taskFolderIndex -lt 0) {
  throw "自定义 has no 浮点 folder in the game's own palette tree (inspect $taskReport)"
}

# 3. Every Float Ops component - the 22 catalogue types and the five M0
#    compatibility probes, which register as "浮点/M0/…" - is inside that folder,
#    and the folder holds nothing else.
$taskTypeIds = @(
  '463332434f4e5331', '4633324144445f31', '4633324449535031', '4633325355425f31',
  '4633324d554c5f31', '4633324449565f31', '4633325351545f31', '4633324e45475f31',
  '4633324142535f31', '463332434d505f31', '463332434c535f31', '463332464d415f31',
  '46333252454d5f31', '463332524e445f31', '4633324d494e5f31', '4633324d41585f31',
  '4633324932465f31', '4633325532465f31', '4633324632495f31', '4633324632555f31',
  '46333253504c5f31', '4633324d4b425f31'
)
$taskProbeIds = @(
  '4633325352433031', '4633325041535331', '4633324455414c31', '463332464c475331',
  '46333253494e4b31'
)
$taskOwnIds = @($taskTypeIds + $taskProbeIds)
$taskInFolder = @()
$taskFolderEnd = $taskLines.Count - 1
for ($taskIndex = $taskFolderIndex; $taskIndex -lt $taskLines.Count; ++$taskIndex) {
  $taskLine = $taskLines[$taskIndex]
  if ($taskIndex -gt $taskFolderIndex -and $taskLine -match '^node=') {
    $taskFolderEnd = $taskIndex - 1
    break
  }
  if ($taskLine -match 'node=.*id=0x([0-9a-f]{16})') { $taskInFolder += $Matches[1] }
}
$taskMissing = @($taskOwnIds | Where-Object { $taskInFolder -notcontains $_ })
if ($taskMissing.Count -gt 0) {
  throw ("the 浮点 folder is missing " + $taskMissing.Count + " component(s): " +
         ($taskMissing -join ', '))
}
$taskExtra = @($taskInFolder | Where-Object { $taskOwnIds -notcontains $_ })
if ($taskExtra.Count -gt 0) {
  throw ("the 浮点 folder holds something that is not a Float Ops component: " +
         ($taskExtra -join ', '))
}

# 4. Nothing is loose: no Float Ops id may appear anywhere else in the tree, so
#    the components are in the folder and not also dumped straight into 自定义
#    (or into another category).
$taskLoose = @()
for ($taskIndex = 0; $taskIndex -lt $taskLines.Count; ++$taskIndex) {
  if ($taskIndex -ge $taskFolderIndex -and $taskIndex -le $taskFolderEnd) { continue }
  if ($taskLines[$taskIndex] -match 'id=0x([0-9a-f]{16})' -and
      $taskOwnIds -contains $Matches[1]) { $taskLoose += $Matches[1] }
}
if ($taskLoose.Count -gt 0) {
  throw ("these components are still listed outside the folder: " + ($taskLoose -join ', '))
}

"PASS float-ops palette: the game built a 浮点 folder inside its own 自定义 category and filed all $($taskOwnIds.Count) float components ($($taskTypeIds.Count) types + $($taskProbeIds.Count) M0 probes) under it; none of them is listed anywhere else in the game's palette tree"
"dump: $(Join-Path $taskRepo 'build\menu.txt')"
"sandbox: $taskTest"
