# A -> B -> A real-game restart test for tc.mod.storage V2 and tc.mod.settings.
param(
  [int]$Seconds = 30,
  [switch]$ReuseSandbox
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskCompiler = if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskSandbox = Join-Path $taskRepo 'build\mod-storage-playtest'
$taskRoot = Join-Path $taskSandbox 'game'
$taskHome = Join-Path $taskSandbox 'home'
$taskId = 'dev.mod-storage-probe'

if ((Test-Path -LiteralPath $taskSandbox) -and -not $ReuseSandbox) {
  $taskBuildRoot = (Resolve-Path -LiteralPath (Join-Path $taskRepo 'build')).Path
  $taskResolved = (Resolve-Path -LiteralPath $taskSandbox).Path
  if (-not $taskResolved.StartsWith($taskBuildRoot)) {
    throw "Refusing to remove a sandbox outside build/: $taskSandbox"
  }
  Remove-Item -LiteralPath $taskSandbox -Recurse -Force
}
New-Item -ItemType Directory -Force $taskRoot,$taskHome,(Join-Path $taskRoot 'mods') | Out-Null
if (-not $ReuseSandbox) {
  foreach($taskDir in @('asset','campaign','translations')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
  }
  foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
  }
  Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
}

$taskPackage = Join-Path $taskRepo 'build\mod-storage-probe-package'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native') | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk (Join-Path $PSScriptRoot 'mod-storage-probe.cpp') -o (Join-Path $taskPackage 'native\probe.dll')
if($LASTEXITCODE){throw 'Mod storage probe build failed'}
'{"format":2,"id":"dev.mod-storage-probe","name":"Mod storage probe","version":"0.1.0","capabilities":["log","services"],"native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding ascii
$taskMod = Join-Path $taskRepo 'dist\dev.mod-storage-probe.mod'
if(Test-Path -LiteralPath $taskMod){Remove-Item -LiteralPath $taskMod}
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output $taskMod | Out-Null
Copy-Item -LiteralPath $taskMod -Destination (Join-Path $taskRoot 'mods') -Force
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply $taskId
if($LASTEXITCODE){throw 'Mod storage sandbox apply failed'}

$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskIni = Join-Path $taskRoot 'tc-modloader-data\saves.ini'
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousOpenSettings = $env:TC_MODLOADER_OPEN_SETTINGS
try {
  $env:USERPROFILE = $taskHome
  $env:APPDATA = Join-Path $taskHome 'AppData\Roaming'
  $env:TC_MODLOADER_OPEN_SETTINGS = '1'
  $taskRuns = @(
    @{Profile='profile-a'; Evidence='mod storage probe: global=1 profile=1'},
    @{Profile='profile-b'; Evidence='mod storage probe: global=2 profile=1'},
    @{Profile='profile-a'; Evidence='mod storage probe: global=3 profile=2'}
  )
  foreach($taskRun in $taskRuns) {
    "[saves]`r`nprofile=$($taskRun.Profile)`r`n" | Set-Content -LiteralPath $taskIni -Encoding ascii -NoNewline
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    try {
      $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
      $taskFound = $false
      while([DateTime]::UtcNow -lt $taskDeadline) {
        Start-Sleep -Milliseconds 250
        if(Test-Path -LiteralPath $taskLog) {
          $taskFound = [bool](Select-String -LiteralPath $taskLog -SimpleMatch $taskRun.Evidence -Quiet)
          if($taskFound){break}
        }
        if($taskProcess.HasExited){break}
      }
      if(-not $taskFound){throw "Missing real-game evidence: $($taskRun.Evidence)"}
    } finally {
      if($taskProcess -and !$taskProcess.HasExited) {
        [void]$taskProcess.CloseMainWindow()
        if(!$taskProcess.WaitForExit(3000)){Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue}
      }
    }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:TC_MODLOADER_OPEN_SETTINGS = $taskPreviousOpenSettings
}

function Read-StoragePayload([string]$Path) {
  $taskBytes = [IO.File]::ReadAllBytes($Path)
  $taskMagic = [Text.Encoding]::ASCII.GetString($taskBytes,0,[Math]::Min(8,$taskBytes.Length))
  if($taskMagic -eq "TCMSV2`r`n") {
    $taskLength = [BitConverter]::ToUInt32($taskBytes,12)
    return [Text.Encoding]::UTF8.GetString($taskBytes,24,$taskLength)
  }
  return [Text.Encoding]::UTF8.GetString($taskBytes)
}
$taskGlobal = Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.mod-storage-probe\storage\runs.bin'
$taskProfileA = Join-Path $taskHome 'AppData\Roaming\Turing Complete Mods\profiles\profile-a\tc-mod-data\dev.mod-storage-probe\runs.bin'
$taskProfileB = Join-Path $taskHome 'AppData\Roaming\Turing Complete Mods\profiles\profile-b\tc-mod-data\dev.mod-storage-probe\runs.bin'
if((Read-StoragePayload $taskGlobal) -ne '3' -or
   (Read-StoragePayload $taskProfileA) -ne '2' -or
   (Read-StoragePayload $taskProfileB) -ne '1') {
  throw 'Persisted storage files do not match the three real-game runs'
}
if(-not (Select-String -LiteralPath $taskLog -SimpleMatch 'Mod settings registered: dev.mod-storage-probe fields=2' -Quiet)) {
  throw 'The declarative settings table was not registered in the real game'
}
if(-not (Select-String -LiteralPath $taskLog -SimpleMatch 'home button settings ' -Quiet)) {
  throw 'The loader did not expose the unified Mod settings page on the home screen'
}
if(-not (Select-String -LiteralPath $taskLog -SimpleMatch 'UI page content tc.modloader/settings ' -Quiet)) {
  throw 'The loader-owned Mod settings page did not draw in the real game'
}
"PASS tc.mod.storage V2 migration + tc.mod.settings page registration across A -> B -> A restarts"
