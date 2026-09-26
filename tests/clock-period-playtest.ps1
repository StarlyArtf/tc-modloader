# Does the clock's corner box really open a value window, and does a number
# typed there land in the instance's configuration?
#
# The unit test (tests/clock-period.cpp) covers the wave, the value parser and
# what the box paints; what it cannot cover is the interaction itself, because
# that needs the game: a real click on the box, a real window, real key
# messages.  So this playtest runs the *driver* build of the clock Mod
# (dist/dev.clock-driver.mod - the same plugin plus tests/clock-period-driver.hpp)
# on a board with a clock on it.  The driver clicks the rectangle the drawing
# code registered for the click, types "8" and Enter, and then checks both the
# period the callback holds and the bytes the instance's configuration carries.
#
# It asserts on the driver's own verdict line, so a red run says what happened
# instead of only that something did not.
param([int]$Seconds = 45)

$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskTest = Join-Path $taskRepo ('build\clock-period-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskSchema = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default\schematics\architecture\Default'
New-Item -ItemType Directory -Force $taskRoot, $taskProfile, (Join-Path $taskRoot 'mods'), $taskSchema | Out-Null
foreach ($taskDirectory in @('asset', 'campaign', 'translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDirectory) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll', 'soft_oal.dll',
                        'steam_api64.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\dev.enter-board.mod') -Destination (Join-Path $taskRoot 'mods\dev.enter-board.mod')
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\dev.clock-driver.mod') -Destination (Join-Path $taskRoot 'mods\dev.clock-driver.mod')
$taskBoard = Join-Path $taskRepo 'build\sandbox_sim_clock_board.data'
if (!(Test-Path -LiteralPath $taskBoard)) {
  throw "Missing $taskBoard (run tools/sandbox-sim-boards.py)"
}
Copy-Item -LiteralPath $taskBoard -Destination (Join-Path $taskSchema 'circuit.data')
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.enter-board dev.clock-driver
if ($LASTEXITCODE) { throw 'Package apply failed' }

$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = (Get-Date).AddSeconds($Seconds)
  do {
    Start-Sleep -Milliseconds 500
    if (Test-Path -LiteralPath $taskLog) {
      $taskSeen = Select-String -LiteralPath $taskLog -Pattern 'CLOCK-DRIVER: (PASS|FAIL)' -ErrorAction SilentlyContinue
      if ($taskSeen) { break }
    }
  } while ((Get-Date) -lt $taskDeadline -and !$taskProcess.HasExited)
  if (!$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
}

if (!(Test-Path -LiteralPath $taskLog)) {
  throw "The game wrote no loader log at $taskLog"
}
$taskText = Get-Content -LiteralPath $taskLog -Raw
Write-Output '--- loader log (clock driver / value window) ---'
Select-String -LiteralPath $taskLog -Pattern 'CLOCK-DRIVER|clock:|value window' |
  Select-Object -Last 12 | ForEach-Object { $_.Line }
if ($taskText -match 'CLOCK-DRIVER: FAIL (.*)') {
  throw "clock value window: $($Matches[1])"
}
if ($taskText -notmatch 'CLOCK-DRIVER: PASS') {
  throw "the driver never reported a verdict; loader log tail:`n" +
        (Get-Content -LiteralPath $taskLog -Tail 10 | Out-String)
}
Write-Output 'clock corner box: the click opened the window and the typed number reached the configuration'
Write-Output "Sandbox: $taskTest"
