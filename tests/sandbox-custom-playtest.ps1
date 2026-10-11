# Real-machine case for custom components (kind 0x4e) in the sandbox simulator.
#
# What it proves: a board carrying the player's own foundry components - each one
# a definition the game keeps in its custom-prototype table - binds and runs in
# simcore.  The definitions are flattened into the board before the netlist is
# built (examples/sandbox-sim/plugin.cpp), so their internals become ordinary
# records and the instance's pins are bridged to the definition's boundary pins.
#
# The board is the player's own sandbox schematic, copied out of their save
# profile together with the foundry definitions and the settings that pick the
# architecture, so the run reproduces the session that used to end in
# "cannot run this board: component 7 has kind 0x4E with no pin geometry".
param(
  [int]$Seconds = 50,
  [string]$SourceProfile = '',
  [switch]$KeepRunning,
  # Leave the board dump on.  The default run is headless (dump=0) and only
  # asserts that the board binds; with -Dump the export the viewer reads is
  # written too, which is how the record format itself gets checked.
  [switch]$Dump,
  # Which Mods to enable in the sandbox.  The default pair is what the case
  # needs; a player's own set (local.float-ops and friends) is what reproduces
  # a crash the default pair cannot, because a Mod-registered prototype is not
  # a circuit definition.
  [string]$Mods = 'dev.sandbox-sim,dev.enter-board',
  # Extra `sandbox-sim.txt` lines, so a run can use the player's own settings
  # (view=1, trace=1, vcd=1, ...) instead of the headless default.  Separated by
  # `;` because `-File` hands the whole argument over as one string.
  [string]$Config = ''
)
$ErrorActionPreference = 'Stop'
$taskMods = @($Mods.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$taskConfig = @($Config.Split(';') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
if (!$SourceProfile) {
  $taskSaveSelection = Join-Path $taskGame 'tc-modloader-data\saves.ini'
  if (!(Test-Path -LiteralPath $taskSaveSelection -PathType Leaf)) {
    throw 'No selected save profile; pass -SourceProfile with the profile directory.'
  }
  $taskSelectedProfile = [regex]::Match((Get-Content -LiteralPath $taskSaveSelection -Raw), '(?m)^profile=([A-Za-z0-9][A-Za-z0-9._-]*)\s*$')
  if (!$taskSelectedProfile.Success) { throw 'Invalid save selection; pass -SourceProfile with the profile directory.' }
  $SourceProfile = Join-Path $env:APPDATA ('Turing Complete Mods\profiles\' + $taskSelectedProfile.Groups[1].Value)
}
foreach ($taskRequired in @((Join-Path $taskRepo 'dist\dev.sandbox-sim.mod'),
                            (Join-Path $taskRepo 'dist\dev.enter-board.mod'),
                            (Join-Path $taskRepo 'dist\tc-loader.dll'),
                            (Join-Path $taskRepo 'dist\tcmod-cli.exe'),
                            (Join-Path $taskGame 'Turing Complete.exe'),
                            (Join-Path $SourceProfile 'schematics'))) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}

$taskSandbox = Join-Path $taskRepo ('build\sandbox-custom-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskSandbox 'game'
$taskProfile = Join-Path $taskSandbox 'home'
$taskProfileDir = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.sandbox-sim'
New-Item -ItemType Directory -Force $taskRoot, $taskProfile, (Join-Path $taskRoot 'mods'), $taskData,
  (Join-Path $taskProfileDir 'schematics') | Out-Null
foreach ($taskDir in @('asset', 'campaign', 'translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll', 'soft_oal.dll',
                        'steam_api64.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
New-Item -ItemType Directory -Force (Join-Path $taskProfileDir 'schematics\sandbox\Default') | Out-Null
Copy-Item -LiteralPath (Join-Path $SourceProfile 'schematics\foundry') -Destination (Join-Path $taskProfileDir 'schematics\foundry') -Recurse
# With an architecture loaded, the sandbox shows that architecture's own board -
# which is the player's CPU, and the board the live session was refused on.
$taskArchitecture = Join-Path $SourceProfile 'schematics\architecture\Symphony'
if (Test-Path -LiteralPath $taskArchitecture) {
  New-Item -ItemType Directory -Force (Join-Path $taskProfileDir 'schematics\architecture') | Out-Null
  Copy-Item -LiteralPath $taskArchitecture -Destination (Join-Path $taskProfileDir 'schematics\architecture\Symphony') -Recurse
}
# The player's own CPU board becomes the sandbox's saved board: that is the board
# the live session was showing when it was refused.  It is a v16 schematic (the
# game's current save format), which is exactly why it is copied rather than
# written.
$taskBoard = Join-Path $SourceProfile 'schematics\architecture\Symphony\circuit.data'
if (!(Test-Path -LiteralPath $taskBoard)) { throw "Missing the player's board: $taskBoard" }
Copy-Item -LiteralPath $taskBoard -Destination (Join-Path $taskProfileDir 'schematics\sandbox\Default\circuit.data') -Force
foreach ($taskSetting in @('settings.txt', 'levels.txt')) {
  $taskFrom = Join-Path $SourceProfile $taskSetting
  if (Test-Path -LiteralPath $taskFrom) { Copy-Item -LiteralPath $taskFrom -Destination $taskProfileDir -Force }
}
$taskSettings = Join-Path $taskProfileDir 'settings.txt'
if (!(Test-Path -LiteralPath $taskSettings)) { throw 'The source profile has no settings.txt' }
$taskSettingsText = Get-Content -LiteralPath $taskSettings -Raw
$taskSettingsText = [regex]::Replace($taskSettingsText, '(?m)^setting_current_level\s*=.*$', 'setting_current_level = sandbox')
if ($taskSettingsText -notmatch 'setting_current_level') {
  $taskSettingsText += "`nsetting_current_level = sandbox`n"
}
[IO.File]::WriteAllText($taskSettings, $taskSettingsText, [Text.UTF8Encoding]::new($false))
@('enabled=1', "dump=$(if ($Dump) { 1 } else { 0 })", 'view=0', 'vcd=0', 'own_cycle=1', 'trace=0') + $taskConfig |
  Set-Content -LiteralPath (Join-Path $taskData 'sandbox-sim.txt') -Encoding ascii
foreach ($taskMod in $taskMods) {
  $taskPackage = Join-Path $taskRepo ('dist\' + $taskMod + '.mod')
  if (!(Test-Path -LiteralPath $taskPackage)) { throw "Missing Mod package: $taskPackage" }
  Copy-Item -LiteralPath $taskPackage -Destination (Join-Path $taskRoot 'mods') -Force
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskMods | Out-Host
if ($LASTEXITCODE) { throw 'The sandbox Mod apply failed' }

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskProcess = $null
$taskLogPath = Join-Path $taskRoot 'tc-modloader-data\loader.log'
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  while ([DateTime]::UtcNow -lt $taskDeadline -and !$taskProcess.HasExited) {
    Start-Sleep -Milliseconds 500
    if (Test-Path -LiteralPath $taskLogPath) {
      $taskSeen = Get-Content -LiteralPath $taskLogPath -Raw
      if ($taskSeen -match 'sandbox-sim: bound board' -or $taskSeen -match 'sandbox-sim: cannot run this board') { break }
    }
  }
  if (!$taskProcess.HasExited) { Start-Sleep -Seconds 5 }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
}

if (!(Test-Path -LiteralPath $taskLogPath)) { throw "No loader log: $taskLogPath" }
$taskLogText = Get-Content -LiteralPath $taskLogPath -Raw
if ($taskLogText -match 'sandbox-sim: cannot run this board') {
  $taskReason = [regex]::Match($taskLogText, 'sandbox-sim: cannot run this board: [^' + "`r`n" + ']*').Value
  throw "simcore refused the board with custom components: $taskReason"
}
if ($taskLogText -notmatch 'sandbox-sim: bound board') { throw 'The sandbox never bound the board' }
$taskDevices = [regex]::Match($taskLogText, 'sandbox-sim: devices -> [^' + "`r`n" + ']*').Value
"PASS sandbox custom components: the player's board with foundry components binds"
"$taskDevices"
"sandbox: $taskSandbox"
