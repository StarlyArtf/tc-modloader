# Development aid (not a catalog case): load one circuit as the sandbox board,
# let the game compile it, and keep the program the game generated.
#
# The game's own compiler is the contract for every component kind - it is what
# turns a board into the program the sandbox would otherwise have to emulate -
# so the cheapest honest way to learn what a kind does is to ask the game to
# compile a board that carries it and read the generated program back.  The
# loader writes one file per compile when TC_MODLOADER_DUMP_SOURCE=1
# (src/native_logic.hpp); this script drives that and collects the files.
#
#   powershell -File tests/kind-source-dump.ps1 -CircuitPath <circuit.data> -Tag register
#
# Result: build/kind-source-<tag>/native-logic-source-*.txt plus the level's own
# component table (sandbox-sim-components.txt) when dev.sandbox-sim bound it.
param(
  [Parameter(Mandatory = $true)][string]$CircuitPath,
  [string]$Tag = 'dump',
  [int]$Seconds = 50,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskCircuit = (Resolve-Path -LiteralPath $CircuitPath).Path
$taskOut = Join-Path $taskRepo ('build\kind-source-' + $Tag)
foreach ($taskRequired in @((Join-Path $taskRepo 'dist\dev.sandbox-sim.mod'),
                            (Join-Path $taskRepo 'dist\dev.enter-board.mod'),
                            (Join-Path $taskRepo 'dist\tc-loader.dll'),
                            (Join-Path $taskRepo 'dist\tcmod-cli.exe'),
                            (Join-Path $taskGame 'Turing Complete.exe'),
                            $taskCircuit)) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}

$taskSandbox = Join-Path $taskRepo ('build\kind-dump-' + $Tag + '-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskSandbox 'game'
$taskProfile = Join-Path $taskSandbox 'home'
$taskProfileDir = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.sandbox-sim'
New-Item -ItemType Directory -Force $taskRoot, $taskProfile, (Join-Path $taskRoot 'mods'), $taskData,
  (Join-Path $taskProfileDir 'schematics\sandbox\Default') | Out-Null
foreach ($taskDir in @('asset', 'campaign', 'translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll', 'soft_oal.dll',
                        'steam_api64.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
# The circuit under study becomes the sandbox's starting board.
Copy-Item -LiteralPath $taskCircuit -Destination (Join-Path $taskRoot 'campaign\sandbox\circuit.data') -Force
@(
  'enabled=1'
  'dump=1'
  'view=0'
  'vcd=0'
  'startup_seed=1'
  'own_cycle=1'
) | Set-Content -LiteralPath (Join-Path $taskData 'sandbox-sim.txt') -Encoding ascii
@(
  'setting_window_mode = true'
  'setting_window_position = 0'
  'setting_window_size = 52430000'
  'setting_language = English'
  'setting_current_level = sandbox'
) | Set-Content -LiteralPath (Join-Path $taskProfileDir 'settings.txt') -Encoding ascii
foreach ($taskPackage in @('dev.sandbox-sim.mod', 'dev.enter-board.mod')) {
  Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\' + $taskPackage)) -Destination (Join-Path $taskRoot 'mods') -Force
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.sandbox-sim dev.enter-board | Out-Host
if ($LASTEXITCODE) { throw 'The sandbox Mod apply failed' }

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousDump = $env:TC_MODLOADER_DUMP_SOURCE
$taskProcess = $null
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  # This is what makes the loader keep the generated program.
  $env:TC_MODLOADER_DUMP_SOURCE = '1'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  while ([DateTime]::UtcNow -lt $taskDeadline -and !$taskProcess.HasExited) {
    Start-Sleep -Milliseconds 500
    $taskSources = @(Get-ChildItem -LiteralPath $taskRoot -Filter 'native-logic-source-*.txt' -ErrorAction SilentlyContinue)
    if ($taskSources.Count -ge 2) { Start-Sleep -Seconds 10; break }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:TC_MODLOADER_DUMP_SOURCE = $taskPreviousDump
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
}

$taskSources = @(Get-ChildItem -LiteralPath $taskRoot -Filter 'native-logic-source-*.txt' -ErrorAction SilentlyContinue | Sort-Object Name)
if (!$taskSources.Count) { throw "The game never compiled the board; inspect $taskSandbox" }
New-Item -ItemType Directory -Force $taskOut | Out-Null
foreach ($taskSource in $taskSources) {
  Copy-Item -LiteralPath $taskSource.FullName -Destination (Join-Path $taskOut $taskSource.Name) -Force
  "$($taskSource.Name): $($taskSource.Length) bytes"
}
foreach ($taskExtra in @('tc-modloader-data\plugin-data\dev.sandbox-sim\sandbox-sim-components.txt',
                         'tc-modloader-data\loader.log')) {
  $taskFrom = Join-Path $taskRoot $taskExtra
  if (Test-Path -LiteralPath $taskFrom) {
    Copy-Item -LiteralPath $taskFrom -Destination (Join-Path $taskOut ([IO.Path]::GetFileName($taskExtra))) -Force
  }
}
"dumps: $taskOut"
"sandbox: $taskSandbox"
