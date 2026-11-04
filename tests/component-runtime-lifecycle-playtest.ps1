$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskCxx = 'C:\msys64\ucrt64\bin\g++.exe'
if ($env:TC_MINGW_BIN) { $taskCxx = Join-Path $env:TC_MINGW_BIN 'g++.exe' }
$taskMod = 'test.runtime-lifecycle'
$taskTest = Join-Path $taskRepo ('build\runtime-lifecycle-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods') | Out-Null
foreach ($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
$taskPackage = Join-Path $taskTest 'probe-package'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native') | Out-Null
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static -shared -I (Join-Path $taskRepo 'sdk') `
  (Join-Path $taskRepo 'tests\component-runtime-lifecycle-probe.cpp') `
  -o (Join-Path $taskPackage 'native\probe.dll')
if ($LASTEXITCODE) { throw 'Runtime lifecycle probe build failed' }
("{`"format`":2,`"id`":`"$taskMod`",`"name`":`"Runtime lifecycle probe`",`"version`":`"0.1.0`",`"capabilities`":[`"log`",`"services`",`"symbol`"]," +
 "`"native`":{`"api`":1,`"entry`":`"native/probe.dll`"}}") |
  Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding ascii
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRoot ("mods\$taskMod.mod")) | Out-Null
if ($LASTEXITCODE) { throw 'Runtime lifecycle probe package failed' }
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply $taskMod | Out-Null
if ($LASTEXITCODE) { throw 'Runtime lifecycle probe apply failed' }

$taskOldProfile = $env:USERPROFILE
$taskOldAppData = $env:APPDATA
$taskProcess = $null
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds(26)
  while ([DateTime]::UtcNow -lt $taskDeadline) {
    Start-Sleep -Milliseconds 500
    if ($taskProcess.HasExited) { break }
  }
} finally {
  $env:USERPROFILE = $taskOldProfile
  $env:APPDATA = $taskOldAppData
  if ($taskProcess -and !$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
}

$taskResult = Join-Path $taskRoot "tc-modloader-data\plugin-data\$taskMod\result.txt"
if (!(Test-Path -LiteralPath $taskResult)) {
  throw "The runtime lifecycle probe produced no result; inspect $taskTest"
}
$taskText = Get-Content -LiteralPath $taskResult -Raw
$taskLines = Get-Content -LiteralPath $taskResult
foreach ($taskExpect in @(
    '1 register       status=0 imported/in-column',
    '2 unregister     status=0 imported/not-in-column',
    '3 register again status=0 imported/in-column',
    '4 register second status=0 imported/in-column',
    '5 replace        status=0 imported/in-column')) {
  if ($taskText -notmatch [regex]::Escape($taskExpect)) {
    throw "Missing step '$taskExpect'; inspect $taskResult"
  }
}
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
if ((Get-Content -LiteralPath $taskLog -Raw) -notmatch
    "put type 0x4c49464543594331 back into the component column's live list") {
  throw "The re-registration was not restored to the column's live list; inspect $taskLog"
}

$taskLines | ForEach-Object { $_ }
"PASS runtime type lifecycle: register, delete, register the same id again (back in the component column's live list), and a live replacement"
"Sandbox: $taskTest"
