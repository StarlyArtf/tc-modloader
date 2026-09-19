# Real-machine playtest for the live waveform panel (example.waveform-demo).
#
# The point of this example is data, not widgets: it has to show the *running
# level's* inputs and outputs, cycle by cycle, as waveforms.  The in-process
# driver (tests/waveform-driver.hpp) enters the stock and_gate level on its own,
# runs it, and reports every row the panel would draw; the plugin then exports
# the same samples as a VCD and the panel itself as a picture.  What it asserts:
#
#   * the panel registers as a board side panel and is drawn on the board only;
#   * the trace sampler resolves the running level's I/O slots (2 in, 2 out for
#     and_gate) and the rows it feeds the panel follow the level's own test:
#     inputs 0,1,2,3 with outputs 0,0,0,1, so an output of 1 appears exactly
#     where the input reaches 3;
#   * "Export VCD" writes a standard VCD with the same signals and values;
#   * the waveform only moves while the simulation does: the driver notes the row
#     count, waits two seconds with the level paused, notes it again, and the two
#     counts have to match; every traced row after that has to be a new cycle;
#   * the picture the panel saves carries the lanes' own colours, i.e. the
#     waveforms really are in the rendered frame, not only in the data.
#
# The picture has to be taken from inside the game: this build runs a borderless
# fullscreen GL window in independent-flip mode, so PrintWindow and desktop grabs
# see nothing of it (measured; the older UI playtests write all-black PNGs for
# exactly that reason).  The plugin reads the OpenGL backbuffer with glReadPixels
# and writes a 32-bit BMP, which this script converts to PNG.
#
# -Example runs the shipped dist/example.waveform-demo.mod instead: that only
# proves the published package registers and loads cleanly (reaching a board
# needs the driver, which is the default mode).
param(
  [string]$Sandbox = (Join-Path (Split-Path $PSScriptRoot) 'build\waveform-sandbox'),
  [int]$Seconds = 60,
  [switch]$Example,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskRoot = Join-Path $Sandbox 'game'
$taskOut = Join-Path $Sandbox 'out'
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskMod = if ($Example) { 'example.waveform-demo' } else { 'dev.waveform-demo-driver' }

# Always refresh the sandbox: make-ui-sandbox.ps1 copies dist\tc-loader.dll over
# game_engine.dll, and a sandbox that keeps an older loader silently tests the
# previous build.
& (Join-Path $PSScriptRoot 'make-ui-sandbox.ps1') -Sandbox $Sandbox -Mods $taskMod
New-Item -ItemType Directory -Force $taskOut | Out-Null
"Loader under test: $((Get-FileHash (Join-Path $taskRoot 'game_engine.dll') -Algorithm SHA256).Hash)"

# and_gate's own test needs a circuit to test, and a fresh sandbox has none:
# install the built-in AND solution into the profile's and_gate slot, exactly
# like tests/sim-trace-probe.ps1 does.  Without it the level has nothing to run
# and the trace never moves off zero.
if (!$Example) {
  $taskSchema = Join-Path $Sandbox 'home\AppData\Roaming\Turing Complete Mods\profiles\default\schematics\and_gate\Default'
  New-Item -ItemType Directory -Force $taskSchema | Out-Null
  $taskSolution = Join-Path $taskRepo 'build\and2_solution_builtin.data'
  if (!(Test-Path -LiteralPath $taskSolution)) { throw "Missing $taskSolution; run build.ps1 first" }
  Copy-Item -LiteralPath $taskSolution -Destination (Join-Path $taskSchema 'circuit.data') -Force
}
Remove-Item -LiteralPath $taskLog -ErrorAction SilentlyContinue

Add-Type -AssemblyName System.Drawing

# Reads a 32-bit BMP into a BGRA byte array so the lane colours can be counted
# without one GetPixel call per pixel.
function Read-BmpPixels {
  param([string]$Path)
  $taskBitmap = [System.Drawing.Bitmap]::FromFile($Path)
  $taskRect = New-Object System.Drawing.Rectangle 0, 0, $taskBitmap.Width, $taskBitmap.Height
  $taskData = $taskBitmap.LockBits($taskRect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
    [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $taskBytes = New-Object byte[] ($taskData.Stride * $taskBitmap.Height)
  [System.Runtime.InteropServices.Marshal]::Copy($taskData.Scan0, $taskBytes, 0, $taskBytes.Length)
  $taskBitmap.UnlockBits($taskData)
  $taskWidth = $taskBitmap.Width
  $taskHeight = $taskBitmap.Height
  $taskStride = $taskData.Stride
  $taskBitmap.Dispose()
  # Drop the row padding so pixel indexing stays simple.
  $taskPixels = New-Object byte[] ($taskWidth * $taskHeight * 4)
  for ($taskY = 0; $taskY -lt $taskHeight; $taskY++) {
    [Array]::Copy($taskBytes, $taskY * $taskStride, $taskPixels, $taskY * $taskWidth * 4, $taskWidth * 4)
  }
  return @{ Pixels = $taskPixels; Width = $taskWidth; Height = $taskHeight }
}

# How many pixels inside $Rect match either lane colour.  The lanes are drawn
# with exactly these colours (tc::ui::rgba(90,170,235) for inputs and
# (245,185,70) for outputs), and the waves are one pixel tall, so every pixel of
# the panel rectangle is inspected - stepping over rows would miss whole lanes.
function Measure-LaneColours {
  param($Image, $Rect, [int]$Tolerance = 16)
  $taskPixels = $Image.Pixels
  $taskInput = 0
  $taskOutput = 0
  for ($taskY = $Rect.Top; $taskY -lt $Rect.Bottom; $taskY++) {
    $taskRow = $taskY * $Image.Width * 4
    for ($taskX = $Rect.Left; $taskX -lt $Rect.Right; $taskX++) {
      $taskIndex = $taskRow + $taskX * 4
      $taskR = [int]$taskPixels[$taskIndex + 2]
      $taskG = [int]$taskPixels[$taskIndex + 1]
      $taskB = [int]$taskPixels[$taskIndex]
      if ([Math]::Abs($taskR - 90) -le $Tolerance -and
          [Math]::Abs($taskG - 170) -le $Tolerance -and
          [Math]::Abs($taskB - 235) -le $Tolerance) {
        $taskInput++
      } elseif ([Math]::Abs($taskR - 245) -le $Tolerance -and
                [Math]::Abs($taskG - 185) -le $Tolerance -and
                [Math]::Abs($taskB - 70) -le $Tolerance) {
        $taskOutput++
      }
    }
  }
  return @{ Input = $taskInput; Output = $taskOutput }
}

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskProcess = $null
$taskText = ''
$taskStop = if ($Example) {
  'Waveform: registered board panel|Native failed|Plugin callback threw|Board panel disabled'
} else {
  'DRIVER: done|DRIVER: giving up|Native failed|Plugin callback threw|Board panel disabled'
}
try {
  $env:USERPROFILE = Join-Path $Sandbox 'home'
  $env:APPDATA = Join-Path $Sandbox 'home\AppData\Roaming'
  # Hidden: the driver enters the level by itself, so no clicks or focus are
  # needed and the game never takes over the desktop.
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = (Get-Date).AddSeconds($Seconds)
  do {
    Start-Sleep -Milliseconds 500
    $taskText = if (Test-Path -LiteralPath $taskLog) { Get-Content -LiteralPath $taskLog -Raw } else { '' }
    if ($taskText -match $taskStop -or $taskProcess.HasExited) { break }
  } while ((Get-Date) -lt $taskDeadline)
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id }
  }
}

if (!(Test-Path -LiteralPath $taskLog)) { throw "Loader log missing: $taskLog" }
$taskLines = Get-Content -LiteralPath $taskLog
$taskText = $taskLines -join "`n"
$taskFatal = $taskLines | Where-Object {
  $_ -match 'Native failed|Plugin callback threw|Board panel disabled|UI 页面异常|回调异常'
}
if ($taskFatal) { $taskFatal | Select-Object -First 8; throw 'The waveform panel reported an error' }

if ($Example) {
  foreach ($taskExpected in @(
      'UI slot registered: waveform (board side panel)',
      'Waveform: registered board panel',
      'Native loaded: example.waveform-demo')) {
    if (!$taskText.Contains($taskExpected)) {
      $taskLines | Select-Object -Last 20
      throw "Missing evidence for the shipped example: $taskExpected"
    }
  }
  ($taskLines | Where-Object { $_ -match 'UI slot registered|Waveform: registered|Native loaded' }) |
    Select-Object -First 4
  'PASS shipped example.waveform-demo: loads and registers its waveform board panel'
  "Sandbox: $Sandbox"
  return
}

if ($taskText -match 'DRIVER: giving up') {
  $taskLines | Select-Object -Last 25
  throw "The driver gave up before the panel traced the level's test sequence"
}
if (!$taskText.Contains('Waveform: registered board panel')) {
  $taskLines | Select-Object -Last 20
  throw 'The waveform panel never registered'
}
if (!($taskLines | Where-Object { $_ -match 'Waveform: panel first drawn on frame \d+' })) {
  $taskLines | Select-Object -Last 20
  throw 'The waveform panel never drew inside the board window'
}

# The rows the panel drew, in order.  They are the level's own test sequence:
# and_gate feeds inputs 0,1,2,3 and expects outputs 0,0,0,1.
$taskRows = @()
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'DRIVER: row (\d+) cycle=(-?\d+) in=([\d,]+) out=([\d,]+)') {
    $taskRows += [pscustomobject]@{
      Order = [int]$Matches[1]
      Cycle = [int]$Matches[2]
      In    = $Matches[3]
      Out   = $Matches[4]
    }
  }
}
# One row per simulation cycle (not per frame), so a short level yields few
# rows: the sequence below is what matters, not the count.
if ($taskRows.Count -lt 3) {
  $taskLines | Select-Object -Last 25
  throw "The panel traced only $($taskRows.Count) rows"
}
$taskFirstIn = [int]($taskRows[0].In -split ',')[0]
$taskLast = $taskRows[$taskRows.Count - 1]
$taskLastIn = [int]($taskLast.In -split ',')[0]
$taskLastOut = [int]($taskLast.Out -split ',')[0]
$taskMaxIn = ($taskRows | ForEach-Object { [int]($_.In -split ',')[0] } | Measure-Object -Maximum).Maximum
$taskHighRows = @($taskRows | Where-Object { [int]($_.Out -split ',')[0] -eq 1 })
if (!$taskHighRows) { throw 'No traced row ever showed a high output' }
# Every high output must be both inputs being high (the level drives value 3 =
# both pins high), and the low outputs come before it.
foreach ($taskRow in $taskHighRows) {
  if ([int]($taskRow.In -split ',')[0] -ne 3) {
    throw "Output was high while the inputs were $($taskRow.In)"
  }
}
if ($taskFirstIn -ne 0) { throw "The first traced input was $taskFirstIn, not 0" }
if ($taskMaxIn -ne 3) { throw "The traced inputs only reached $taskMaxIn" }
if ($taskLastIn -ne 3 -or $taskLastOut -ne 1) {
  throw "The trace ended at in=$taskLastIn out=$taskLastOut"
}
# A paused simulation must not add rows: the driver measured the row count, left
# the game paused for two seconds and measured again.
$taskPaused = $taskLines | Where-Object { $_ -match 'DRIVER: paused rows=(\d+)' } | Select-Object -First 1
$taskPausedLater = $taskLines | Where-Object { $_ -match 'DRIVER: paused rows after 2s=(\d+)' } | Select-Object -First 1
if (!$taskPaused -or !$taskPausedLater) {
  $taskLines | Select-Object -Last 20
  throw 'The driver never reported the paused row counts'
}
$null = $taskPaused -match 'DRIVER: paused rows=(\d+)'
$taskPausedRows = [int]$Matches[1]
$null = $taskPausedLater -match 'DRIVER: paused rows after 2s=(\d+)'
$taskPausedRowsLater = [int]$Matches[1]
if ($taskPausedRows -ne $taskPausedRowsLater) {
  throw "The waveform grew while the simulation was paused: $taskPausedRows -> $taskPausedRowsLater rows"
}
# ... and every row after that is a new simulation cycle, not a render frame.
for ($taskIndex = 1; $taskIndex -lt $taskRows.Count; $taskIndex++) {
  if ($taskRows[$taskIndex].Cycle -le $taskRows[$taskIndex - 1].Cycle) {
    throw ("Row $($taskRows[$taskIndex].Order) repeats cycle $($taskRows[$taskIndex].Cycle): " +
           'the waveform is not following the simulation')
  }
}
# Wire probe: the driver asks the panel to probe the wire on the level's output
# side, then reports the probe's value next to the level's own output for every
# row.  They have to agree - that is what proves the probe reads *that wire*.
$taskProbeRows = @()
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'DRIVER: probe row=(\d+) cycle=(-?\d+) value=(\d+) out0=(\d+)') {
    $taskProbeRows += [pscustomobject]@{
      Row = [int]$Matches[1]; Cycle = [int]$Matches[2]
      Value = [int]$Matches[3]; Output = [int]$Matches[4]
    }
  }
}
if ($taskProbeRows.Count -lt 3) {
  $taskLines | Select-Object -Last 20
  throw "The wire probe produced only $($taskProbeRows.Count) comparable rows"
}
foreach ($taskProbe in $taskProbeRows) {
  if ($taskProbe.Value -ne $taskProbe.Output) {
    throw ("Probed wire disagrees with the level's output at cycle $($taskProbe.Cycle): " +
           "probe=$($taskProbe.Value) out=$($taskProbe.Output)")
  }
}
if (!($taskProbeRows | Where-Object { $_.Value -eq 1 })) {
  throw 'The probed wire never went high'
}
$taskProbePicked = $taskLines | Where-Object { $_ -match 'Waveform: driver probes wire (\d+) state@(\d+) width=(\d+)' } |
  Select-Object -First 1
if (!$taskProbePicked) { throw 'The panel never accepted the probed wire' }

# The VCD the panel's Export button wrote has to carry the same signals.
$taskExport = $taskLines | Where-Object { $_ -match 'DRIVER: export rows=(\d+) path=(.+)$' } |
  Select-Object -First 1
if (!$taskExport) {
  $taskLines | Select-Object -Last 20
  throw 'The VCD export never finished'
}
$null = $taskExport -match 'DRIVER: export rows=(\d+) path=(.+)$'
$taskExportRows = [int]$Matches[1]
$taskVcdPath = $Matches[2].Trim()
if ($taskExportRows -lt 3) { throw "The export only had $taskExportRows rows" }
if (!(Test-Path -LiteralPath $taskVcdPath)) { throw "The exported VCD is missing: $taskVcdPath" }
$taskVcd = Get-Content -LiteralPath $taskVcdPath -Raw
foreach ($taskExpected in @('$timescale', '$var wire 64 i0 in0', '$var wire 64 o1 out1', '#3')) {
  if (!$taskVcd.Contains($taskExpected)) { throw "The VCD is missing $taskExpected" }
}
$taskSignals = ([regex]::Matches($taskVcd, '\$var wire 64')).Count
if ($taskSignals -ne 4) { throw "The VCD declares $taskSignals signals, expected 4" }
# 3 = both and_gate inputs high; the output goes high there and stays high.
if ($taskVcd -notmatch '(?m)^b11 i0\r?$') { throw 'The VCD never shows input 0 reaching 3' }
if ($taskVcd -notmatch '(?m)^b1 o0\r?$') { throw 'The VCD never shows a high output' }

# The picture the panel saved from inside the game: the waveforms have to be in
# the rendered frame with their own colours, not only in the data.
$taskImage = $taskLines | Where-Object { $_ -match 'DRIVER: image (.+)$' } | Select-Object -First 1
if (!$taskImage) {
  $taskLines | Select-Object -Last 20
  throw 'The panel never saved its framebuffer picture'
}
$null = $taskImage -match 'DRIVER: image (.+)$'
$taskBmpPath = $Matches[1].Trim()
if (!(Test-Path -LiteralPath $taskBmpPath)) { throw "The captured picture is missing: $taskBmpPath" }
$taskPng = Join-Path $taskOut 'waveform.png'
$taskBmp = [System.Drawing.Bitmap]::FromFile($taskBmpPath)
$taskBmp.Save($taskPng, [System.Drawing.Imaging.ImageFormat]::Png)
$taskBmp.Dispose()
# The panel rectangle comes from the loader's own log line, so the colour count
# is taken exactly where the panel was drawn.
$taskPanelLine = $taskLines | Where-Object {
  $_ -match 'Board panel .*?/waveform frame=\d+ x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)'
} | Select-Object -Last 1
if (!$taskPanelLine) { throw 'The loader never logged the panel rectangle' }
$null = $taskPanelLine -match 'x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)'
$taskPanel = New-Object System.Drawing.Rectangle ([int]$Matches[1]), ([int]$Matches[2]),
  ([int]$Matches[3]), ([int]$Matches[4])
$taskImageData = Read-BmpPixels $taskBmpPath
$taskLaneColours = Measure-LaneColours $taskImageData $taskPanel
$taskInputPixels = $taskLaneColours.Input
$taskOutputPixels = $taskLaneColours.Output
# With one row per cycle a short level draws very few pixels, so the floor only
# has to prove the lanes are really there.
if ($taskInputPixels -lt 10) {
  throw "The input lane colour appears on only $taskInputPixels pixels inside the panel"
}
if ($taskOutputPixels -lt 10) {
  throw "The output lane colour appears on only $taskOutputPixels pixels inside the panel"
}

$taskRows | Select-Object -First 6 | Format-Table -AutoSize | Out-String -Width 120 | Write-Host
$taskExport
"Paused rows: $taskPausedRows -> $taskPausedRowsLater (no growth while paused)"
"Wire probe: $($taskProbePicked -replace '^.*Waveform: ', '') - value matched the level's own output on all " +
"$($taskProbeRows.Count) rows"
"Rows the panel drew (one per new cycle): $($taskRows.Count) (inputs $taskFirstIn..$taskMaxIn, " +
"$($taskHighRows.Count) high-output rows)"
"VCD: $taskVcdPath ($taskExportRows rows, $taskSignals signals)"
"Picture: $taskPng ($($taskImageData.Width)x$($taskImageData.Height); input-lane pixels " +
"$taskInputPixels, output-lane pixels $taskOutputPixels inside the panel at " +
"$($taskPanel.X),$($taskPanel.Y) $($taskPanel.Width)x$($taskPanel.Height))"
"PASS waveform panel: the board panel traced the running level's inputs and outputs per cycle "
"(and_gate: inputs $taskFirstIn..$taskMaxIn, output low then high where both inputs are high), "
"added no rows during a two-second pause and one row per new cycle while running, "
"drew them while the level was up, exported them as a $taskSignals-signal VCD with the same values, "
"and the rendered frame really contains both lane colours"
"Sandbox: $Sandbox"
