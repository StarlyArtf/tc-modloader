# Real-machine case for the "WordWatchee 64" fix package.
#
# It installs the package into a throwaway copy of the game and checks the two
# halves of the fix where they actually happen:
#
#   * the exact text patch rewrites the game's own shader copy (and the pinned
#     installation is left alone, so the assertion cannot pass by accident);
#   * the plugin raises the renderer's value_size ceiling inside the running
#     game - the byte it read back is the evidence, not a promise;
#   * the game itself starts and keeps rendering with the patched code; a frame
#     is captured as the artifact a person can look at.
#
# The value-size ceiling is one byte in the game's own function, so "the game
# still runs" is part of the test: a wrong address would take the process down.
param(
  [int]$Seconds = 45,
  [int]$ShotDelay = 14000,
  # Which loader to install as game_engine.dll.  The default is the build in
  # dist/; pointing this at an older loader (or at the engine a player already
  # has installed) is how the package is checked against it.
  [string]$Loader = ''
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskModId = 'local.word-watchee-64'
$taskShader = 'asset\shader\word_watchee.vert'
$taskFrag = 'asset\shader\word_watchee.frag'
$taskTest = Join-Path $taskRepo ('build\word-watchee-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot "tc-modloader-data\plugin-data\$taskModId"
$taskResult = Join-Path $taskData 'result.txt'
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskShot = Join-Path $taskTest 'frame.bmp'

New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods') | Out-Null
if (!$Loader) { $Loader = Join-Path $taskRepo 'dist\tc-loader.dll' }
if (!(Test-Path -LiteralPath $Loader)) { throw "Loader not found: $Loader" }
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath $Loader -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath (Join-Path $taskRepo "dist\$taskModId.mod") -Destination (Join-Path $taskRoot 'mods')

# A second, throwaway package whose only job is to get the game from the home
# page into a board (tests/enter-board.cpp): a screenshot of the main menu does
# not show that the patched renderer still draws circuits.
$taskCompiler = if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskBoardProbe = 'test.enter-board'
$taskProbePackage = Join-Path $taskTest 'enter-board-package'
New-Item -ItemType Directory -Force (Join-Path $taskProbePackage 'native') | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -I (Join-Path $taskRepo 'sdk') (Join-Path $PSScriptRoot 'enter-board.cpp') -o (Join-Path $taskProbePackage 'native\enter-board.dll')
if($LASTEXITCODE){throw 'Enter-board probe build failed'}
("{`"format`":2,`"id`":`"$taskBoardProbe`",`"name`":`"Enter board probe`",`"version`":`"0.1.0`",`"native`":{`"api`":1,`"entry`":`"native/enter-board.dll`"}}") | Set-Content -LiteralPath (Join-Path $taskProbePackage 'mod.json') -Encoding ascii
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskProbePackage -Output (Join-Path $taskRoot ('mods\' + $taskBoardProbe + '.mod')) | Out-Null

$taskProcess = $null
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousShot = $env:TC_MODLOADER_SHOT
$taskPreviousShotDelay = $env:TC_MODLOADER_SHOT_DELAY
$taskPreviousTrace = $env:TC_WATCHEE_TRACE
try {
  & (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply $taskModId $taskBoardProbe
  if($LASTEXITCODE){throw 'Word watchee 64 package apply failed'}

  # 1) The two shader files the package ships are the ones on disk: byte for byte
  #    against the archive's own copies, so "the fix is in the sandbox" and "the
  #    package is what the game reads" are the same statement.
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $taskZip = [IO.Compression.ZipFile]::OpenRead((Join-Path $taskRepo "dist\$taskModId.mod"))
  try {
    foreach($taskRelative in @('files/asset/shader/word_watchee.vert','files/asset/shader/word_watchee.frag')) {
      $taskEntry = $taskZip.Entries | Where-Object { $_.FullName -eq $taskRelative }
      if(!$taskEntry){throw "The package has no $taskRelative"}
      $taskStream = New-Object IO.MemoryStream
      $taskEntry.Open().CopyTo($taskStream)
      $taskPacked = [Security.Cryptography.SHA256]::Create().ComputeHash($taskStream.ToArray())
      $taskStream.Dispose()
      $taskPackedHash = (($taskPacked | ForEach-Object { $_.ToString('x2') }) -join '')
      $taskDeployedPath = Join-Path $taskRoot ($taskRelative -replace '^files/','' -replace '/','\')
      if(!(Test-Path -LiteralPath $taskDeployedPath)){throw "Not deployed: $taskDeployedPath"}
      $taskDeployedHash = (Get-FileHash -LiteralPath $taskDeployedPath -Algorithm SHA256).Hash.ToLowerInvariant()
      if($taskPackedHash -ne $taskDeployedHash){throw "Deployed shader differs from the package: $taskRelative"}
    }
  } finally { $taskZip.Dispose() }
  $taskPatchedShader = Get-Content -LiteralPath (Join-Path $taskRoot $taskShader) -Raw
  $taskPatchedFrag = Get-Content -LiteralPath (Join-Path $taskRoot $taskFrag) -Raw
  if($taskPatchedShader -notmatch 'shift = 28;'){throw 'The deployed vertex shader has no 64-bit low-word shift fix'}
  if($taskPatchedShader -notmatch 'label_rows = 2\.0;'){throw 'The deployed vertex shader does not split wide labels'}
  if($taskPatchedShader -notmatch 'current / 10u'){throw 'The deployed vertex shader has no exact divide by ten'}
  if($taskPatchedShader -match '0xCCCDu'){throw 'The deployed vertex shader still uses the game inverse multiply'}
  if($taskPatchedShader -match 'scratch\['){throw 'The deployed vertex shader still indexes a local array dynamically'}
  if($taskPatchedFrag -notmatch 'int\(row\) \* ROW_STRIDE \+ int\(whole\)'){throw 'The deployed fragment shader does not address lines'}
  $taskState = Get-Content -LiteralPath (Join-Path $taskRoot 'tc-modloader-data\state.json') -Raw | ConvertFrom-Json
  foreach($taskRelative in @('asset/shader/word_watchee.vert','asset/shader/word_watchee.frag')) {
    $taskFile = $taskState.files.$taskRelative
    if(!$taskFile){throw "state.json does not record $taskRelative"}
    # original and deployed may be equal when the sandbox was cloned from an
    # install that already carried this exact file; what matters is that both
    # hashes are recorded and the original content is preserved in blobs/.
    if(!(Test-Path -LiteralPath (Join-Path $taskRoot ('tc-modloader-data\blobs\' + $taskFile.original)))) {
      throw "The original $taskRelative was not backed up"
    }
  }
  "PASS shaders deployed from the package: vert=$(($taskState.files.'asset/shader/word_watchee.vert'.deployed).Substring(0,12)) frag=$(($taskState.files.'asset/shader/word_watchee.frag'.deployed).Substring(0,12))"

  # 2) Start the game once, so the plugin meets the real function in memory.
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $env:TC_MODLOADER_SHOT = $taskShot
  $env:TC_MODLOADER_SHOT_DELAY = "$ShotDelay"
  $env:TC_WATCHEE_TRACE = $null
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
    Start-Sleep -Milliseconds 250
  }
  if($taskProcess.HasExited) { throw "The game exited during startup (exit code $($taskProcess.ExitCode)); inspect $taskLog" }
  if(!(Test-Path -LiteralPath $taskResult)) { throw "The plugin never reported; inspect $taskLog" }

  # 3) What the plugin says, and what it actually read back.
  $taskText = [IO.File]::ReadAllText($taskResult).Trim()
  $taskText
  if(!$taskText.StartsWith('PASS ')) { throw "Word watchee 64 plugin failed: $taskText" }
  foreach($taskExpected in @('before=mov eax,0x20','after=0x40','ceiling=64')) {
    if(!$taskText.Contains($taskExpected)) { throw "Missing plugin evidence: $taskExpected" }
  }
  # The probe presses a home page entry a few seconds in; wait for the board so
  # the patched renderer really draws a circuit before this case ends.
  $taskDeadline = [DateTime]::UtcNow.AddSeconds(45)
  while([DateTime]::UtcNow -lt $taskDeadline) {
    $taskLines = Get-Content -LiteralPath $taskLog
    if($taskLines | Where-Object { $_ -match 'ENTER-BOARD: pressed a home page entry to reach a board' }) { break }
    if($taskProcess.HasExited) { throw "The game exited before reaching a board (exit code $($taskProcess.ExitCode))" }
    Start-Sleep -Milliseconds 500
  }
  $taskLines = Get-Content -LiteralPath $taskLog
  $taskRequired = @(
      'value_size ceiling 32 -> 64 at 0x.* \(mov eax,0x20; read back 64\)',
      'ENTER-BOARD: pressed a home page entry to reach a board')
  # report_status is a 0.6.0 entry point, so the Mods page line only exists on a
  # loader that has it; an older loader still gets the log line and the file.
  if($taskLines | Where-Object { $_ -match 'TC Mod Loader 0\.6' }) {
    $taskRequired += 'word-watchee-64: value labels keep all 64 bits'
  }
  foreach($taskExpected in $taskRequired) {
    if(!($taskLines | Where-Object { $_ -match $taskExpected })) {
      $taskLines | Where-Object { $_ -match 'word-watchee|ENTER-BOARD' } | Select-Object -Last 5
      throw "Missing loader log evidence: $taskExpected"
    }
  }
  if($taskLines | Where-Object { $_ -match 'Native failed:|Native runtime unavailable|Failed to create shader|Shift held' }) {
    throw 'The loader log reports a shader or plugin failure'
  }
  $taskFaultLog = Join-Path $taskRoot 'tc-modloader-data\fault.log'
  if((Test-Path -LiteralPath $taskFaultLog) -and (Get-Item -LiteralPath $taskFaultLog).Length -gt 0) {
    throw "A fault was recorded: $(Get-Content -LiteralPath $taskFaultLog -Raw)"
  }

  # 4) The frame capture: proof that the patched engine kept drawing.
  $taskDeadline = [DateTime]::UtcNow.AddSeconds(20)
  while(!(Test-Path -LiteralPath $taskShot) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
    Start-Sleep -Milliseconds 250
  }
  if(!(Test-Path -LiteralPath $taskShot)) { throw 'No frame was captured; the renderer did not survive the patch' }
  Add-Type -AssemblyName System.Drawing
  $taskImage = [System.Drawing.Image]::FromFile($taskShot)
  $taskPng = Join-Path $taskTest 'frame.png'
  $taskImage.Save($taskPng, [System.Drawing.Imaging.ImageFormat]::Png)
  $taskImage.Dispose()
  if((Get-Item -LiteralPath $taskShot).Length -lt 1024) { throw 'The captured frame is empty' }
  "PASS frame captured: $taskPng"
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:TC_MODLOADER_SHOT = $taskPreviousShot
  $env:TC_MODLOADER_SHOT_DELAY = $taskPreviousShotDelay
  $env:TC_WATCHEE_TRACE = $taskPreviousTrace
  if($taskProcess -and !$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
  }
  "Evidence: $taskTest"
}
