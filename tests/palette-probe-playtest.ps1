# Runs the palette-category probe together with the Float Ops Mod in an isolated
# sandbox, and prints what the game's component palette groups by.
param(
  [int]$Seconds = 15,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskCxx = Join-Path $taskCompiler 'g++.exe'
$taskId = 'test.palette-probe'
$taskMods = @('test.palette-probe', 'local.float-ops')
$taskTest = Join-Path $taskRepo ('build\palette-probe-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot ('tc-modloader-data\plugin-data\' + $taskId)
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
New-Item -ItemType Directory -Force $taskRoot, $taskProfile, (Join-Path $taskRoot 'mods'), $taskData | Out-Null
foreach ($taskDir in @('asset', 'campaign', 'translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll', 'soft_oal.dll',
                         'steam_api64.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') `
  -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\local.float-ops.mod') `
  -Destination (Join-Path $taskRoot 'mods') -Force

# The probe itself, built here so it always matches the source next to it.
$taskPackage = Join-Path $taskTest 'probe-package'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native') | Out-Null
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static -shared -I (Join-Path $taskRepo 'sdk') `
  (Join-Path $PSScriptRoot 'palette-probe.cpp') -o (Join-Path $taskPackage 'native\probe.dll')
if ($LASTEXITCODE) { throw 'The palette probe did not compile' }
("{`"format`":2,`"id`":`"$taskId`",`"name`":`"Palette category probe`",`"version`":`"0.1.0`"," +
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
$taskPreviousBodies = $env:TC_FLOATOPS_BODIES
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  # The probe needs the game's own names: the palette is drawn from prototypes,
  # which exist as soon as the game is up.
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  while ([DateTime]::UtcNow -lt $taskDeadline) {
    Start-Sleep -Milliseconds 500
    if ($taskProcess.HasExited) { break }
    if (Test-Path -LiteralPath (Join-Path $taskData 'palette.txt')) { break }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:TC_FLOATOPS_BODIES = $taskPreviousBodies
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
}

$taskReport = Join-Path $taskData 'palette.txt'
if (!(Test-Path -LiteralPath $taskReport)) {
  throw "The probe wrote no palette.txt; inspect $taskLog"
}
$taskOut = Join-Path $taskRepo 'build\palette.txt'
Copy-Item -LiteralPath $taskReport -Destination $taskOut -Force
Get-Content -LiteralPath $taskOut | Select-Object -First 40
"...(full dump: $taskOut)"
"Sandbox: $taskTest"
