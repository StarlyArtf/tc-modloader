# Real-machine case for the punch tape's runtime wide constants.
#
# The 1-second stall the player saw was not UI work: every click asked the game
# to recompile the whole board because a wide constant was baked into the
# generated source as a literal.  The loader now emits those constants as
# runtime lookups instead, so this case checks the half that only a real level
# can answer:
#
#   * a real level with a wide constant produces source containing
#     game_engine.'tc_dynamic_constant' in both the refresh and cycle phase;
#   * the game's own compiler accepts that source and keeps running (the level's
#     sim reaches a cycle, and no fault is recorded);
#   * the punch tape is still drawn over the wide constant's drawer, captured as
#     a frame a person can look at.
#
# The driver is test-only (tests/punch-tape-select-driver.cpp): it selects the
# constant so the drawer opens without needing the camera.
param(
  [int]$Seconds = 120,
  [int]$ShotDelay = 55000,
  # Level whose schematic carries wide constants.  'sandbox' is the free-build
  # level under the player's own save, which is where the stall was measured.
  [string]$Level = 'sandbox',
  # A profile directory (levels.txt + schematics) to copy into the sandbox.  The
  # player's own board lives in one: loading the level it belongs to is the only
  # way to test the runtime constants against the circuit that showed the stall.
  [string]$Profile = (Join-Path $env:APPDATA 'Turing Complete'),
  # The loader only writes native-logic-source-*.txt when asked to; this case
  # reads those dumps for its code-generation evidence, so it turns them on.
  # -NoDumpEvidence checks the other half: with the variable unset the game
  # directory stays clean while the tape still works.
  [switch]$NoDumpEvidence,
  # Which loader to install as game_engine.dll.  Pointing this at an older
  # loader is how the same case is run against the pre-change build.
  [string]$Loader = ''
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskTest = Join-Path $taskRepo ('build\punch-tape-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskFault = Join-Path $taskRoot 'tc-modloader-data\fault.log'
$taskShot = Join-Path $taskTest 'frame.bmp'
$taskCompiler = if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskDriverId = 'test.punch-runtime'
$taskMods = @('example.byte-adder', 'local.punch-tape', $taskDriverId)

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
foreach($taskMod in @('example.byte-adder','local.punch-tape')) {
  $taskSource = Join-Path $taskRepo ('dist\' + $taskMod + '.mod')
  if(!(Test-Path -LiteralPath $taskSource)) { throw "Missing Mod package: $taskSource" }
  Copy-Item -LiteralPath $taskSource -Destination (Join-Path $taskRoot 'mods') -Force
}
# The driver is built here rather than taken from dist/, so this case always
# runs the driver that matches the source next to it.
$taskDriverPackage = Join-Path $taskTest 'driver-package'
New-Item -ItemType Directory -Force (Join-Path $taskDriverPackage 'native') | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Werror -static -shared `
  -I (Join-Path $taskRepo 'sdk') (Join-Path $PSScriptRoot 'punch-tape-runtime-driver.cpp') `
  -o (Join-Path $taskDriverPackage 'native\driver.dll')
if($LASTEXITCODE){throw 'Punch tape runtime driver build failed'}
("{`"format`":2,`"id`":`"$taskDriverId`",`"name`":`"Punch tape runtime driver`",`"version`":`"0.1.0`",`"native`":{`"api`":1,`"entry`":`"native/driver.dll`"}}") |
  Set-Content -LiteralPath (Join-Path $taskDriverPackage 'mod.json') -Encoding ascii
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskDriverPackage `
  -Output (Join-Path $taskRoot ('mods\' + $taskDriverId + '.mod')) | Out-Null
if($LASTEXITCODE){throw 'Punch tape runtime driver pack failed'}
if(!(Test-Path -LiteralPath (Join-Path $taskRoot ('mods\' + $taskDriverId + '.mod')))) {
  throw 'The driver package was not written'
}
# The byte-adder example doubles as the headless level loader: its autotest
# reads this file and drives load_level + compile + run (examples/byte-adder).
$taskDriverData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.byte-adder'
New-Item -ItemType Directory -Force $taskDriverData | Out-Null
Set-Content -LiteralPath (Join-Path $taskDriverData 'autotest.txt') -Value $Level -Encoding ascii
if ($Profile) {
  if (!(Test-Path -LiteralPath $Profile)) { throw "Profile not found: $Profile" }
  $taskProfileTarget = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
  New-Item -ItemType Directory -Force (Split-Path $taskProfileTarget) | Out-Null
  Copy-Item -LiteralPath $Profile -Destination $taskProfileTarget -Recurse -Force
}

$taskProcess = $null
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousShot = $env:TC_MODLOADER_SHOT
$taskPreviousShotDelay = $env:TC_MODLOADER_SHOT_DELAY
$taskPreviousDump = $env:TC_MODLOADER_DUMP_SOURCE
try {
  & (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskMods
  if($LASTEXITCODE){throw 'Punch tape sandbox apply failed'}

  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $env:TC_MODLOADER_SHOT = $taskShot
  $env:TC_MODLOADER_SHOT_DELAY = "$ShotDelay"
  $env:TC_MODLOADER_DUMP_SOURCE = if($NoDumpEvidence){ '0' } else { '1' }
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  while([DateTime]::UtcNow -lt $taskDeadline) {
    Start-Sleep -Milliseconds 500
    if($taskProcess.HasExited) { break }
    if(Test-Path -LiteralPath $taskShot) { break }
  }
  if($taskProcess.HasExited) { throw "The game exited during the case (exit code $($taskProcess.ExitCode)); inspect $taskLog" }
  if(!(Test-Path -LiteralPath $taskLog)) { throw 'No loader log: the game did not start' }

  $taskLines = Get-Content -LiteralPath $taskLog
  $taskRequired = @(
    "byte-adder: autotest loaded level $Level",
    'punch tape: runtime wide constants enabled',
    'punch tape: constant drawer hook installed',
    'PUNCHDRV: armed; runtime setter present',
    'PUNCHDRV: constant index='
  )
  foreach($taskExpected in $taskRequired) {
    if(!($taskLines | Where-Object { $_ -match [regex]::Escape($taskExpected) })) {
      $taskLines | Select-Object -Last 12
      throw "Missing loader log evidence: $taskExpected"
    }
  }
  if($taskLines | Where-Object { $_ -match 'Native failed:|compile failed|Error: |unhandled exception' }) {
    $taskLines | Where-Object { $_ -match 'failed|Error' } | Select-Object -Last 8
    throw 'The loader log reports a failure'
  }
  if((Test-Path -LiteralPath $taskFault) -and (Get-Item -LiteralPath $taskFault).Length -gt 0) {
    throw "A fault was recorded: $(Get-Content -LiteralPath $taskFault -Raw)"
  }

  # The generated source is the primary evidence: both emission phases have to
  # read the runtime slot instead of a literal.  Without the dumps (-NoDumpEvidence)
  # the point of this run is the opposite one: nothing may be written at all.
  $taskDumps = Get-ChildItem -LiteralPath $taskRoot -Filter 'native-logic-source-*.txt' -File
  "generated source dumps: $($taskDumps.Count)"
  if($NoDumpEvidence) {
    if($taskDumps.Count -ne 0) {
      $taskDumps | Select-Object -First 5 Name
      throw "The loader wrote $($taskDumps.Count) source dump(s) with TC_MODLOADER_DUMP_SOURCE unset"
    }
    if(!($taskLines | Where-Object { $_ -match 'PUNCHDRV: no compiled source carries the constant' })) {
      throw 'The driver should have reported that it had no generated source to read'
    }
    "PASS the game directory stayed clean: no native-logic-source-*.txt"
  }
  $taskRefresh = @()
  $taskCycle = @()
  if(!$NoDumpEvidence) {
    foreach($taskDump in $taskDumps) {
      $taskRefresh += Select-String -LiteralPath $taskDump.FullName -Pattern "let value_id\d+ = \w+ game_engine\.'tc_dynamic_constant'" -AllMatches
      $taskCycle += Select-String -LiteralPath $taskDump.FullName -Pattern "var vid\d+ = \w+ game_engine\.'tc_dynamic_constant'" -AllMatches
    }
    "runtime constant lines: refresh=$($taskRefresh.Count) cycle=$($taskCycle.Count)"
    if($taskDumps.Count -eq 0) { throw 'The game never compiled a schematic in the sandbox' }
    if($taskRefresh.Count -eq 0) { throw 'No refresh-phase line reads the runtime constant' }
    if($taskCycle.Count -eq 0) { throw 'No cycle-phase line reads the runtime constant' }
    $taskRefresh[0].Line.Trim()
    $taskCycle[0].Line.Trim()
  }

  # The level's own sim has to keep running: a rejected source would never reach
  # a cycle, and the autotest would never log its verdict.
  if(!($taskLines | Where-Object { $_ -match 'byte-adder: autotest finished' })) {
    throw 'The level never reached a finished simulation'
  }
  $taskLines | Where-Object { $_ -match 'autotest (loaded level|finished)' } | Select-Object -Last 4
  $taskLines | Where-Object { $_ -match 'DRIVER:|punch tape:' } | Select-Object -Last 8

  # The update sequence itself: no recompile may happen, so the number of
  # generated-source dumps has to stay put across the three updates.
  $taskClicks = @($taskLines | Where-Object { $_ -match 'PUNCHDRV: sequence \d value=\d+ took=[\d.]+ ms' })
  if($taskClicks.Count -lt 3) {
    $taskLines | Where-Object { $_ -match 'PUNCHDRV:' } | Select-Object -Last 8
    throw "The driver reported $($taskClicks.Count) update sequence(s), expected 3"
  }
  $taskClicks | ForEach-Object { ($_ -replace '^.*PUNCHDRV: ','') }
  $taskDumpCounts = @($taskLines | Where-Object { $_ -match 'PUNCHDRV: dumps (before|after)=\d+' } |
    ForEach-Object { [int]($_ -replace '^.*=','') })
  if($taskDumpCounts.Count -lt 2) { throw 'The driver did not report its source-dump counts' }
  if($taskDumpCounts[0] -ne $taskDumpCounts[1]) {
    throw "The update sequence recompiled the board: dumps $($taskDumpCounts[0]) -> $($taskDumpCounts[1])"
  }
  "no recompile: source dumps stayed at $($taskDumpCounts[0]) across three updates"

  # Real mouse clicks on the tape's own squares, posted to the game window.
  $taskPattern = 'PUNCHDRV: realclick bit=(\d+) roundtrip=([\d.]+) ms panel index=(\d+) bit=(\d+) value=(\d+)'
  $taskRealClicks = @($taskLines | Where-Object { $_ -match $taskPattern })
  if($taskRealClicks.Count -lt 3) {
    # The tape only exists while the game's drawer is showing the constant, and
    # that drawer is opened by the player's own click on the board.  When the
    # drawer never came up this case still reports what it could measure.
    "note: $($taskRealClicks.Count) real click(s) landed on the tape (3 wanted; the drawer has to be open)"
  } else {
    foreach($taskClick in $taskRealClicks) {
      $taskMatch = [regex]::Match($taskClick, $taskPattern)
      if($taskMatch.Groups[1].Value -ne $taskMatch.Groups[4].Value) {
        throw "The tape toggled bit $($taskMatch.Groups[4].Value) when bit $($taskMatch.Groups[1].Value) was clicked"
      }
    }
    $taskRealClicks | ForEach-Object { ($_ -replace '^.*PUNCHDRV: ','') }
  }
  $taskClickDumps = @($taskLines | Where-Object { $_ -match 'PUNCHDRV: realclick dumps before=\d+ after=\d+' } |
    ForEach-Object { [int]($_ -replace '^.*=','') })
  if($taskClickDumps.Count -ge 2) {
    if($taskClickDumps[0] -ne $taskClickDumps[1]) {
      throw "A real tape click recompiled the board: dumps $($taskClickDumps[0]) -> $($taskClickDumps[1])"
    }
    "no recompile: source dumps stayed at $($taskClickDumps[0]) across three real clicks"
  }
  if($NoDumpEvidence) {
    "PASS punch tape: the tape worked with no generated-source dump written"
    if(Test-Path -LiteralPath $taskShot) {
      Add-Type -AssemblyName System.Drawing
      $taskImage = [System.Drawing.Image]::FromFile($taskShot)
      $taskPng = Join-Path $taskTest 'frame.png'
      $taskImage.Save($taskPng, [System.Drawing.Imaging.ImageFormat]::Png)
      $taskImage.Dispose()
      "Frame: $taskPng"
    }
    return
  }

  # The value the circuit now computes, read through the game's own state reader
  # at the offset the generated refresh program stores the constant to.  This is
  # what separates "the drawer shows a new number" from "the simulation uses it".
  $taskStates = @($taskLines | Where-Object { $_ -match 'PUNCHDRV: state at \d+ = \d+ expected \d+ (MATCH|MISMATCH)' })
  if($taskStates.Count -eq 0) {
    $taskLines | Where-Object { $_ -match 'PUNCHDRV:' } | Select-Object -Last 6
    throw 'The driver never read back what the circuit computed'
  }
  $taskStates | ForEach-Object { ($_ -replace '^.*PUNCHDRV: ','') }
  foreach($taskState in $taskStates) {
    if($taskState -notmatch 'MATCH$') { throw "The circuit did not compute the value the tape set: $taskState" }
  }
  "the circuit computed the value the tape set"

  if(!(Test-Path -LiteralPath $taskShot)) { throw 'No frame was captured: the game stopped rendering' }
  if((Get-Item -LiteralPath $taskShot).Length -lt 1024) { throw 'The captured frame is empty' }
  Add-Type -AssemblyName System.Drawing
  $taskImage = [System.Drawing.Image]::FromFile($taskShot)
  $taskPng = Join-Path $taskTest 'frame.png'
  $taskImage.Save($taskPng, [System.Drawing.Imaging.ImageFormat]::Png)
  $taskImage.Dispose()
  "PASS punch tape runtime constants: the level compiled, ran and kept the tape"
  "Frame: $taskPng"
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:TC_MODLOADER_SHOT = $taskPreviousShot
  $env:TC_MODLOADER_SHOT_DELAY = $taskPreviousShotDelay
  $env:TC_MODLOADER_DUMP_SOURCE = $taskPreviousDump
  if($taskProcess -and !$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
  "Evidence: $taskTest"
}
