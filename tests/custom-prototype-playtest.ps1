# Development aid (not a catalog case): run tests/custom-prototype-probe.cpp in
# an isolated copy of the game and keep the raw custom prototypes it dumps.
#
# Why: a custom component (kind 0x4e) is a definition the game parsed out of a
# circuit, and simcore needs that circuit to evaluate the component.  The
# prototype is where the game keeps it.  This run copies the player's own
# `schematics/foundry` into the sandbox so the game registers exactly those
# definitions, then asks the probe for the ones the board actually uses.
#
#   powershell -File tests/custom-prototype-playtest.ps1 -FoundryProfile <profile-directory> -Ids <hex-id-list>
param(
  [int]$Seconds = 30,
  # The custom ids to dump, comma separated, in hex.
  [Parameter(Mandatory)][string]$Ids,
  # Where the player's foundry definitions live (their own save profile).
  [Parameter(Mandatory)][string]$FoundryProfile
)
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskOut = Join-Path $taskRepo 'build\custom-prototype-dump'
if (!(Test-Path -LiteralPath (Join-Path $taskGame 'Turing Complete.exe'))) { throw 'Missing the game' }

$taskSandbox = Join-Path $taskRepo ('build\custom-prototype-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskSandbox 'game'
$taskProfile = Join-Path $taskSandbox 'home'
$taskProfileDir = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.custom-prototype'
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
# The player's own foundry definitions, so the game registers the same custom
# types their board uses.
$taskFoundry = Join-Path $FoundryProfile 'schematics\foundry'
if (Test-Path -LiteralPath $taskFoundry) {
  Copy-Item -LiteralPath $taskFoundry -Destination (Join-Path $taskProfileDir 'schematics\foundry') -Recurse
  "foundry definitions: $((Get-ChildItem (Join-Path $taskProfileDir 'schematics\foundry') -Directory).Count)"
} else {
  "note: no foundry folder at $taskFoundry"
}
@(
  'setting_window_mode = true'
  'setting_window_position = 0'
  'setting_window_size = 52430000'
  'setting_language = English'
) | Set-Content -LiteralPath (Join-Path $taskProfileDir 'settings.txt') -Encoding ascii
# The probe as a Mod, packaged exactly like the others.
$taskPackage = Join-Path $taskRepo 'build\custom-prototype-package'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native') | Out-Null
Copy-Item -LiteralPath (Join-Path $taskRepo 'build\custom-prototype-probe.dll') `
  -Destination (Join-Path $taskPackage 'native\custom-prototype.dll') -Force
'{"format":2,"id":"dev.custom-prototype","name":"Custom prototype dump","version":"0.1.0","native":{"api":1,"entry":"native/custom-prototype.dll"}}' |
  Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding utf8
if (Test-Path -LiteralPath (Join-Path $taskRepo 'build\custom-prototype.mod')) {
  Remove-Item -LiteralPath (Join-Path $taskRepo 'build\custom-prototype.mod') -Force
}
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRepo 'build\custom-prototype.mod') | Out-Null
Copy-Item -LiteralPath (Join-Path $taskRepo 'build\custom-prototype.mod') -Destination (Join-Path $taskRoot 'mods') -Force
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.custom-prototype | Out-Host
if ($LASTEXITCODE) { throw 'The probe could not be enabled' }

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousDump = $env:TC_CUSTOM_DUMP
$taskProcess = $null
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $env:TC_CUSTOM_DUMP = $Ids
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds $Seconds
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:TC_CUSTOM_DUMP = $taskPreviousDump
  if ($taskProcess -and !$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
}

$taskDumps = @(Get-ChildItem -LiteralPath $taskData -Filter 'custom-prototype-*.txt' -ErrorAction SilentlyContinue)
New-Item -ItemType Directory -Force $taskOut | Out-Null
foreach ($taskDump in $taskDumps) {
  Copy-Item -LiteralPath $taskDump.FullName -Destination (Join-Path $taskOut $taskDump.Name) -Force
}
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
if (Test-Path -LiteralPath $taskLog) {
  Select-String -LiteralPath $taskLog -Pattern 'custom-prototype:' | ForEach-Object { $_.Line.Trim() }
}
"dumps: $taskOut ($($taskDumps.Count) file(s))"
"sandbox: $taskSandbox"
