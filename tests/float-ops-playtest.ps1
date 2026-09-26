param(
  [int]$Seconds = 90,
  [int]$ShotDelay = 23000,
  [string]$Level = 'not_gate',
  [switch]$KeepRunning
)

# M2 true-game closure (docs/PLAN-float-components.md, sections 6.1, 7 and 10.2).
#
# The board is two FP32 Constants feeding an FP32 Add whose R[32] drives an FP32
# Display.  Everything the case needs goes through paths the shipped Mod uses:
# the driver writes each instance's configuration with tc.component.storage
# (the same call the editors make), the arithmetic runs the SoftFloat kernel,
# and the text is read back from the Display's own cache.  Three configurations
# are checked in order: the registered defaults, 3.5 + 1.25, and the halfway
# case 1.0 + 2**-25 where RNE and RUP have to disagree.

$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskCxx = Join-Path $taskCompiler 'g++.exe'
$taskId = [guid]::NewGuid().ToString('N')
$taskDriverBuild = Join-Path $taskRepo ('build\float-m2-driver-' + $taskId)
$taskDriverStage = Join-Path $taskDriverBuild 'package'
$taskDriverNative = Join-Path $taskDriverStage 'native'
$taskDriverMod = Join-Path $taskDriverBuild 'local.float-ops.mod'
$taskBoard = Join-Path $taskDriverBuild 'float-m2-board.data'
$taskSandbox = Join-Path $taskRepo ('build\float-m2-' + $taskId)
$taskRoot = Join-Path $taskSandbox 'game'
$taskProfile = Join-Path $taskSandbox 'home'
$taskMods = Join-Path $taskRoot 'mods'
$taskLoader = Join-Path $taskRepo 'dist\tc-loader.dll'
$taskCli = Join-Path $taskRepo 'dist\tcmod-cli.exe'
$taskLevel = $Level
$taskSchema = Join-Path $taskProfile `
  "AppData\Roaming\Turing Complete Mods\profiles\default\schematics\$taskLevel\Default"
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\local.float-ops'
$taskShot = Join-Path $taskDriverBuild 'float-m2-board.bmp'

foreach ($taskRequired in @($taskCxx, $taskLoader, $taskCli,
                             (Join-Path $taskGame 'Turing Complete.exe'))) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}

# The real plugin object plus the test-only level runner, exactly like the M0
# and M1 cases: the driver is never part of the production package.
& (Join-Path $taskRepo 'examples\float-ops\build-kernel.ps1') | Out-Host
if ($LASTEXITCODE) { throw 'Float Ops kernel build failed' }
$taskKernelObjects = @(Get-Content -LiteralPath `
  (Join-Path $taskRepo 'build\float-ops\float-kernel-objects.rsp'))
$taskKernelFlags = @(Get-Content -LiteralPath `
  (Join-Path $taskRepo 'build\float-ops\float-kernel-includes.rsp'))

New-Item -ItemType Directory -Force $taskDriverNative | Out-Null
$taskPluginObject = Join-Path $taskDriverBuild 'float-ops.o'
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror @taskKernelFlags `
  -Dtc_mod_load=float_ops_m0_embedded_load -c `
  (Join-Path $taskRepo 'examples\float-ops\plugin.cpp') -o $taskPluginObject
if ($LASTEXITCODE) { throw 'Float Ops plugin object did not compile' }

$taskDriverDll = Join-Path $taskDriverNative 'float-compat-driver.dll'
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -Wno-unused-function `
  -Wno-unused-variable -static -static-libgcc -static-libstdc++ -shared `
  @taskKernelFlags (Join-Path $PSScriptRoot 'float-compat-driver.cpp') `
  $taskPluginObject $taskKernelObjects -o $taskDriverDll
if ($LASTEXITCODE) { throw 'Float Ops true-game driver did not compile' }

$taskManifest = @'
{
  "format": 2,
  "id": "local.float-ops",
  "name": "Float Ops M2 true-game driver",
  "version": "0.2.0",
  "capabilities": ["log", "status", "services", "game_handles", "symbol", "hook", "logic", "component"],
  "native": {"api": 1, "entry": "native/float-compat-driver.dll"}
}
'@
Set-Content -LiteralPath (Join-Path $taskDriverStage 'mod.json') -Value $taskManifest -Encoding ascii
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskDriverStage -Output $taskDriverMod | Out-Null

$taskFixtureExe = Join-Path $taskDriverBuild 'float-fixture.exe'
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static `
  (Join-Path $PSScriptRoot 'float-fixture.cpp') -o $taskFixtureExe
if ($LASTEXITCODE) { throw 'Float Ops fixture generator did not compile' }
& $taskFixtureExe --m2 $taskBoard | Out-Host
if ($LASTEXITCODE -or !(Test-Path -LiteralPath $taskBoard)) {
  throw 'The M2 fixture was not generated'
}

New-Item -ItemType Directory -Force $taskRoot,$taskProfile,$taskMods,$taskSchema,$taskData | Out-Null
foreach ($taskDir in @('asset', 'campaign', 'translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll',
                         'soft_oal.dll', 'steam_api64.dll', 'libgcc_s_seh-1.dll',
                         'libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath $taskLoader -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath $taskDriverMod -Destination (Join-Path $taskMods 'local.float-ops.mod')
Copy-Item -LiteralPath $taskBoard -Destination (Join-Path $taskSchema 'circuit.data')
Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value $taskLevel -Encoding ascii

& $taskCli $taskRoot apply local.float-ops | Out-Host
if ($LASTEXITCODE) { throw 'Float Ops isolated apply failed' }

$taskDoneMarker = 'float-ops M2 test: done'
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousShot = $env:TC_MODLOADER_SHOT
$taskPreviousShotDelay = $env:TC_MODLOADER_SHOT_DELAY
$taskPreviousLayout = $env:TC_FLOATOPS_LAYOUT
$taskProcess = $null
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  # The board picture is the only place the *look* of a component can be
  # measured: the layout case reads it back and checks the body's size, the
  # value's box, the label's corner and that the pins are outside the body.
  $env:TC_MODLOADER_SHOT = $taskShot
  $env:TC_MODLOADER_SHOT_DELAY = "$ShotDelay"
  # Ask the Mod for a layout line per frame, so the picture can be matched to
  # the frame it was captured from.
$env:TC_FLOATOPS_LAYOUT = '1'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  do {
    Start-Sleep -Milliseconds 500
    $taskText = if (Test-Path -LiteralPath $taskLog) {
      Get-Content -LiteralPath $taskLog -Raw
    } else { '' }
    if ($taskText.Contains($taskDoneMarker) -or
        $taskText.Contains('float-ops M2: registering') -or
        $taskText.Contains('Native failed') -or $taskProcess.HasExited) { break }
  } while ([DateTime]::UtcNow -lt $taskDeadline)
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:TC_MODLOADER_SHOT = $taskPreviousShot
  $env:TC_MODLOADER_SHOT_DELAY = $taskPreviousShotDelay
  $env:TC_FLOATOPS_LAYOUT = $taskPreviousLayout
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) {
      Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue
    }
  }
}

if (!(Test-Path -LiteralPath $taskLog)) { throw "No loader log; inspect $taskSandbox" }
$taskLines = Get-Content -LiteralPath $taskLog
$taskText = $taskLines -join "`n"
$taskEvidence = @($taskLines | Where-Object {
  $_ -match 'float-ops M2|float-ops M1: kernel self-check|Native logic: registered custom 0x463332(43|41|44)'
})
$taskEvidence | Select-Object -First 40 | ForEach-Object { $_ }

# The three types have to be registered with every service the editors need. 
$taskServices = 'float-ops M2: constant/add/display registered; render=ok geometry=ok ' +
  'storage=ok instances=ok save=ok ui=ok mouse=ok'
if (!$taskText.Contains($taskServices)) {
  throw "The M2 components did not arm with every service; inspect $taskLog"
}
if (!$taskText.Contains('float-ops M1: kernel self-check 1+2=0x40400000/0x00')) {
  throw "The M1 kernel self-check is missing; inspect $taskLog"
}

foreach ($taskExpected in @(
    'float-ops M2 test: 3.5 and 1.25 written to the constants (ok)',
    'float-ops M2 test: right after the write, with no run, display shows "4.75 0x40980000"',
    'float-ops M2 test: after the constants were set, display shows "4.75 0x40980000"',
    'float-ops M2 test: halfway operands and RNE written (ok)',
    'float-ops M2 test: with RNE, display shows "1 0x3F800000"',
    'float-ops M2 test: rounding box set to RUP (ok)',
    'float-ops M2 test: with RUP, display shows "1.0000001 0x3F800001"')) {
  if (!$taskText.Contains($taskExpected)) {
    throw "Missing M2 evidence: $taskExpected (inspect $taskLog)"
  }
}

"PASS float-ops M2 vertical slice: constant configuration -> FP32 Add (RNE then RUP) -> display text; 4.75 and the halfway case 1.0+2^-25 both reached the board"

# ---- the look: the board picture, measured against the stock parts --------- #
#
# `docs/research/component-appearance.md` and the reference capture of an
# original CONST give the numbers a Float Ops component has to match: a body
# 4.92 x 2.93 cells, the value 0.59 cells tall in the middle of it, the name
# 0.40 cells tall in the top right corner, the width badge in the top left and
# the pin 3.0 cells out - *outside* the body.  The Mod logs where each type
# landed (float-ops render: type=… unit=… body=… pin0=…), so this case measures
# the picture it captured instead of assuming where anything is.
if (!(Test-Path -LiteralPath $taskShot)) {
  throw "The board picture was not captured; inspect $taskShot and $taskLog"
}

# The instance's own label reaches the board: the driver writes "left" into the
# first constant and the render callback reports what the name slot shows.
if (!$taskText.Contains('name="left"')) {
  throw "The component label did not reach the board; inspect $taskLog"
}

# The editor lives in the game's own component drawer: selecting one of the
# Mod's components is what opens it, and the Mod says so when the drawer shows
# one of its instances.  A floating window of the Mod's own is gone (the panel
# screenshot below is the game's panel, with the Mod's rows in it).
if (!$taskText.Contains('float-ops: the editor lives in the game''s component drawer')) {
  throw "The Mod did not register its rows in the component drawer; inspect $taskLog"
}
# The drawer's own rows are exercised by selecting a component, which needs a
# real click at a real moment: the sandbox window is started hidden, so a
# synthetic press only lands when the game happens to have the pointer (the
# click that used to do this here also opened the game's own value popup over
# the Display, which the pixel case below then measured).  What this case can
# assert without input is that the Mod registered its editor for the game's
# component panel, and that the loader's host for it is armed; the rows
# themselves are in the panel screenshot of the sandbox
# (build/float-m2-<id>/game) whenever a run does select one - see
# docs/verification.md.
if (!$taskText.Contains('Component panel: the game''s component drawer is hooked for Mod editor rows')) {
  throw "The loader did not hook the game's component drawer; inspect $taskLog"
}
Add-Type -AssemblyName System.Drawing
$taskImage = [System.Drawing.Bitmap]::FromFile($taskShot)
function Get-TaskInkRows {
  param([System.Drawing.Bitmap]$Bitmap,[double]$X0,[double]$Y0,[double]$X1,[double]$Y1)
  $rows = @()
  for ($y = [int][Math]::Floor($Y0); $y -le [int][Math]::Ceiling($Y1); $y++) {
    $count = 0; $minimum = 0; $maximum = -1
    for ($x = [int][Math]::Floor($X0); $x -le [int][Math]::Ceiling($X1); $x++) {
      if ($x -lt 0 -or $y -lt 0 -or $x -ge $Bitmap.Width -or $y -ge $Bitmap.Height) { continue }
      $pixel = $Bitmap.GetPixel($x, $y)
      # Board text is the only near-white thing inside a body: the fill, the
      # border, the badge and the board's own background are all far darker.
      if ($pixel.R -gt 190 -and $pixel.G -gt 190 -and $pixel.B -gt 190) {
        $count++
        if ($maximum -lt 0 -or $x -lt $minimum) { $minimum = $x }
        if ($x -gt $maximum) { $maximum = $x }
      }
    }
    $rows += [pscustomobject]@{ Y = $y; Count = $count; Min = $minimum; Max = $maximum }
  }
  ,$rows
}
# The badge: the Mod paints it in its own dark fill, which no other pixel inside
# a body uses, so its box is measurable whatever text is around it.
function Get-TaskBadgeBox {
  param([System.Drawing.Bitmap]$Bitmap,[double]$X0,[double]$Y0,[double]$X1,[double]$Y1)
  $minX = 0; $maxX = -1; $minY = 0; $maxY = -1
  for ($y = [int][Math]::Floor($Y0); $y -le [int][Math]::Ceiling($Y1); $y++) {
    for ($x = [int][Math]::Floor($X0); $x -le [int][Math]::Ceiling($X1); $x++) {
      if ($x -lt 0 -or $y -lt 0 -or $x -ge $Bitmap.Width -or $y -ge $Bitmap.Height) { continue }
      $pixel = $Bitmap.GetPixel($x, $y)
      if ([Math]::Abs($pixel.R - 28) -le 10 -and [Math]::Abs($pixel.G - 32) -le 10 -and
          [Math]::Abs($pixel.B - 36) -le 10) {
        if ($maxX -lt 0) { $minX = $x; $maxX = $x; $minY = $y; $maxY = $y; continue }
        if ($x -lt $minX) { $minX = $x }
        if ($x -gt $maxX) { $maxX = $x }
        if ($y -lt $minY) { $minY = $y }
        if ($y -gt $maxY) { $maxY = $y }
      }
    }
  }
  if ($maxX -lt 0) { return $null }
  [pscustomobject]@{ MinX = $minX; MinY = $minY; MaxX = $maxX; MaxY = $maxY }
}
# The last layout written before the picture was taken is the frame in the
# picture: the Mod traces one line per instance per frame while
# TC_FLOATOPS_LAYOUT is set, and the camera may have moved earlier on.
$taskCaptureIndex = -1
for ($taskIndex = 0; $taskIndex -lt $taskLines.Count; $taskIndex++) {
  if ($taskLines[$taskIndex] -match 'Captured frame screenshot') { $taskCaptureIndex = $taskIndex }
}
$taskLayoutLines = @()
for ($taskIndex = 0; $taskIndex -lt $taskLines.Count; $taskIndex++) {
  if ($taskCaptureIndex -ge 0 -and $taskIndex -gt $taskCaptureIndex) { continue }
  if ($taskLines[$taskIndex] -match 'float-ops render: type=') {
    $taskLayoutLines += $taskLines[$taskIndex]
  }
}
$taskLayouts = @{}
foreach ($taskLine in $taskLayoutLines) {
  if ($taskLine -match 'float-ops render: type=0x([0-9a-f]+) unit=([-0-9.]+) px/cell body=([-0-9.]+),([-0-9.]+)\.\.([-0-9.]+),([-0-9.]+) pin0=([-0-9.]+),([-0-9.]+) value="([^"]*)"') {
    $taskLayouts[$Matches[1]] = [pscustomobject]@{
      Unit = [double]$Matches[2]
      MinX = [double]$Matches[3]; MinY = [double]$Matches[4]
      MaxX = [double]$Matches[5]; MaxY = [double]$Matches[6]
      PinX = [double]$Matches[7]; PinY = [double]$Matches[8]
      Value = $Matches[9]
    }
  }
}
if ($taskLayouts.Count -lt 3) {
  throw "The Mod did not report a layout for all three types; inspect $taskLog"
}
# 0x463332434f4e5331 Constant, 0x4633324144445f31 Add, 0x4633324449535031 Display.
$taskValueCentred = @('463332434f4e5331')
$taskValueSized = @('463332434f4e5331', '4633324449535031')
foreach ($taskEntry in $taskLayouts.GetEnumerator()) {
  $taskType = $taskEntry.Key
  $taskLayout = $taskEntry.Value
  $taskUnit = $taskLayout.Unit
  $taskBodyWidth = $taskLayout.MaxX - $taskLayout.MinX
  $taskBodyHeight = $taskLayout.MaxY - $taskLayout.MinY
  if ([Math]::Abs($taskBodyWidth / $taskUnit - 4.92) -gt 0.20) {
    throw "type 0x$taskType body is $([Math]::Round($taskBodyWidth / $taskUnit,2)) cells wide, the stock part is 4.92"
  }
  if ([Math]::Abs($taskBodyHeight / $taskUnit - 2.93) -gt 0.20) {
    throw "type 0x$taskType body is $([Math]::Round($taskBodyHeight / $taskUnit,2)) cells tall, the stock part is 2.93"
  }
  # The pin has to be outside the body it belongs to - the whole point of the
  # 3.0 lane - and clear of the edge rather than overlapping it.
  $taskCentreX = ($taskLayout.MinX + $taskLayout.MaxX) * 0.5
  $taskCentreY = ($taskLayout.MinY + $taskLayout.MaxY) * 0.5
  $taskPinOut = [Math]::Abs($taskLayout.PinX - $taskCentreX) - $taskBodyWidth * 0.5
  if ($taskPinOut -lt 0.20 * $taskUnit -or $taskPinOut -gt 0.85 * $taskUnit) {
    throw "type 0x$taskType pin sits $([Math]::Round($taskPinOut / $taskUnit,2)) cells from the body edge, the stock part is 0.54"
  }
  if ([Math]::Abs($taskLayout.PinY - $taskCentreY) -gt 0.25 * $taskUnit) {
    throw "type 0x$taskType pin is not on the body's own row"
  }
  # Width badge in the top left corner, the stock size and inset.
  # Strictly inside the body: a neighbouring part's badge sits outside this one
  # and would otherwise be measured as part of it.
  $taskBadge = Get-TaskBadgeBox -Bitmap $taskImage -X0 ($taskLayout.MinX + 1) `
    -Y0 ($taskLayout.MinY + 1) `
    -X1 ($taskLayout.MinX + 1.25 * $taskUnit) -Y1 $taskCentreY
  if (!$taskBadge) { throw "type 0x$taskType draws no width badge" }
  $taskBadgeLeft = $taskBadge.MinX - $taskLayout.MinX
  $taskBadgeTop = $taskBadge.MinY - $taskLayout.MinY
  $taskBadgeWidth = $taskBadge.MaxX - $taskBadge.MinX
  $taskBadgeHeight = $taskBadge.MaxY - $taskBadge.MinY
  if ($taskBadgeLeft -lt 0.02 * $taskUnit -or $taskBadgeLeft -gt 0.40 * $taskUnit -or
      $taskBadgeTop -lt 0.02 * $taskUnit -or $taskBadgeTop -gt 0.40 * $taskUnit) {
    throw ("type 0x$taskType width badge is inset $([Math]::Round($taskBadgeLeft / $taskUnit,2))," +
           "$([Math]::Round($taskBadgeTop / $taskUnit,2)) cells, the stock part is 0.17,0.17")
  }
  if ([Math]::Abs($taskBadgeWidth / $taskUnit - 1.00) -gt 0.25 -or
      [Math]::Abs($taskBadgeHeight / $taskUnit - 0.66) -gt 0.20) {
    throw ("type 0x$taskType width badge is $([Math]::Round($taskBadgeWidth / $taskUnit,2))x" +
           "$([Math]::Round($taskBadgeHeight / $taskUnit,2)) cells, the stock part is 1.00x0.66")
  }
  $taskRows = Get-TaskInkRows -Bitmap $taskImage -X0 ($taskLayout.MinX + 1) `
    -Y0 ($taskLayout.MinY + 1) -X1 ($taskLayout.MaxX - 1) -Y1 ($taskLayout.MaxY - 1)
  $taskInked = @($taskRows | Where-Object { $_.Count -ge 2 })
  if ($taskInked.Count -lt 4) { throw "type 0x$taskType draws no readable text" }
  # One row of text is the contiguous band of inked scan lines around its first
  # inked pixel.
  function Get-TaskInkBand {
    param($AllRows,[int]$SeedY)
    $low = $SeedY; $high = $SeedY
    while ($true) {
      $row = @($AllRows | Where-Object { $_.Y -eq ($low - 1) -and $_.Count -ge 1 })
      if (!$row) { break }
      $low--
    }
    while ($true) {
      $row = @($AllRows | Where-Object { $_.Y -eq ($high + 1) -and $_.Count -ge 1 })
      if (!$row) { break }
      $high++
    }
    $band = @($AllRows | Where-Object { $_.Y -ge $low -and $_.Y -le $high })
    [pscustomobject]@{
      MinY = $low; MaxY = $high
      MinX = ($band | Where-Object { $_.Max -ge 0 } | Measure-Object -Property Min -Minimum).Minimum
      MaxX = ($band | Measure-Object -Property Max -Maximum).Maximum
      Inked = ($band | Measure-Object -Property Count -Sum).Sum
    }
  }
  # The name: the top row of text, in the top right corner.
  $taskLabel = Get-TaskInkBand -AllRows $taskRows -SeedY $taskInked[0].Y
  $taskLabelHeight = $taskLabel.MaxY - $taskLabel.MinY + 1
  # The band is the whole top row of text - the name, the width digits inside
  # the badge and, on the adder, the rounding code - so it is measured with the
  # same tolerance as the value rather than to a tenth of a cell: the stock
  # part's name is 0.40 cells tall, and anti-aliased edges add a row.
  if ([Math]::Abs($taskLabelHeight / $taskUnit - 0.40) -gt 0.20) {
    throw "type 0x$taskType name is $([Math]::Round($taskLabelHeight / $taskUnit,2)) cells tall, the stock part is 0.40"
  }
  $taskLabelInset = $taskLayout.MaxX - $taskLabel.MaxX
  if ($taskLabelInset -lt 0.05 * $taskUnit -or $taskLabelInset -gt 0.45 * $taskUnit) {
    throw "type 0x$taskType name ends $([Math]::Round($taskLabelInset / $taskUnit,2)) cells from the right edge, the stock part is 0.21"
  }
  $taskLabelTop = $taskLabel.MinY - $taskLayout.MinY
  if ($taskLabelTop -gt 0.45 * $taskUnit) {
    throw "type 0x$taskType name starts $([Math]::Round($taskLabelTop / $taskUnit,2)) cells below the top edge, the stock part is 0.17"
  }
  # The value or operator: the tallest row of text below the name's own row.  On
  # a display the bit pattern sits under the value and is the longer of the two,
  # so "busiest" would measure the wrong row; the value is the one the stock
  # part sizes at 0.59 cells.
  $taskValueRows = @($taskInked | Where-Object { $_.Y -gt $taskLabel.MaxY })
  if ($taskValueRows.Count -lt 2) { throw "type 0x$taskType draws no value below the name" }
  $taskValue = $null
  foreach ($taskRow in $taskValueRows) {
    $taskBand = Get-TaskInkBand -AllRows $taskRows -SeedY $taskRow.Y
    $taskBandHeight = $taskBand.MaxY - $taskBand.MinY + 1
    if (!$taskValue -or $taskBandHeight -gt ($taskValue.MaxY - $taskValue.MinY + 1)) {
      $taskValue = $taskBand
    }
  }
  $taskValueHeight = $taskValue.MaxY - $taskValue.MinY + 1
  if ($taskValueSized -contains $taskType) {
    $taskValueCells = $taskValueHeight / $taskUnit
    # A value the Mod had to shrink to fit its body is measured against "it
    # fits and stays readable", not against the stock 0.59 cells: the board
    # writes a long value ("2.9802322e-08") at a size that fits inside the body.
    if ($taskLayout.Value.Length -le 6) {
      if ([Math]::Abs($taskValueCells - 0.59) -gt 0.14) {
        throw "type 0x$taskType value `"$($taskLayout.Value)`" is $([Math]::Round($taskValueCells,2)) cells tall, the stock part is 0.59"
      }
    } elseif ($taskValueCells -lt 0.22 -or $taskValueCells -gt 0.62) {
      throw "type 0x$taskType value `"$($taskLayout.Value)`" is $([Math]::Round($taskValueCells,2)) cells tall, which neither fits the body nor stays readable"
    }
  }
  if ($taskValueCentred -contains $taskType) {
    $taskValueCentre = ($taskValue.MinX + $taskValue.MaxX) * 0.5
    if ([Math]::Abs($taskValueCentre - $taskCentreX) -gt 0.30 * $taskUnit) {
      throw "type 0x$taskType value is not centred horizontally"
    }
    $taskValueCentreY = ($taskValue.MinY + $taskValue.MaxY) * 0.5
    if ([Math]::Abs($taskValueCentreY - $taskCentreY) -gt 0.30 * $taskUnit) {
      throw "type 0x$taskType value is not centred vertically"
    }
  }
  "PASS float-ops render type=0x$taskType unit=$([Math]::Round($taskUnit,2))px " +
    "body=$([Math]::Round($taskBodyWidth / $taskUnit,2))x$([Math]::Round($taskBodyHeight / $taskUnit,2)) cells " +
    "badge=$([Math]::Round($taskBadgeWidth / $taskUnit,2))x$([Math]::Round($taskBadgeHeight / $taskUnit,2)) at " +
    "$([Math]::Round($taskBadgeLeft / $taskUnit,2)),$([Math]::Round($taskBadgeTop / $taskUnit,2)) " +
    "name=$([Math]::Round($taskLabelHeight / $taskUnit,2)) cells inset $([Math]::Round($taskLabelInset / $taskUnit,2)) " +
    "value=$([Math]::Round($taskValueHeight / $taskUnit,2)) cells pin outside by $([Math]::Round($taskPinOut / $taskUnit,2)) cells"
}
$taskImage.Dispose()

"Sandbox: $taskSandbox"
