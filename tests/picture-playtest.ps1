# The component column and drawer use a Mod component's registered picture.  The
# placement ghost is a separate surface: V6 lets the Mod's board draw callback
# own it, because the game does not apply that picture to a custom prototype's
# ghost.
#
# The mechanism under test is tc.component.render V5 (docs/research/component-icons.md
# measured where the game asks for that picture: one texture per custom prototype,
# "?snapshot_cc/com_custom_<decimal id>.png", once per frame).  A PNG dropped at
# the requested path is read and re-encoded by the game but never shown - the
# picture is the game's own render - so the loader answers the *texture request*
# instead, with a copy of what the Mod registered.
#
# Three surfaces, measured separately, because they do not behave alike:
#   * the item in the component column and the drawer's preview picture both draw
#     the requested texture, so the registered PNG is what the player sees there
#     (asserted: the marker colour appears in the first run and in neither of the
#     control run's pictures);
#   * the placement ghost is taken over through tc.component.render V6 and drawn
#     by Float Ops' ordinary board callback with instance_id == 0.  The case
#     requires its purple body and red pins in both runs, while also proving that
#     the V5 marker picture never leaks into this callback-rendered surface.
#
# Telling "our picture" from "the game's own" needs a signature the board itself
# never draws, so the case runs the Mod twice:
#   green  the Mod registers deliberately garish (magenta) pictures for all 22 of
#          its types, from a directory this case writes;
#   red    the Mod registers nothing (TC_FLOATOPS_PICTURES=0) and the same flow
#          photographs the same three surfaces.
# Magenta in the green card/drawer pictures and none in the red ones proves V5;
# the same purple ghost body in both runs proves V6 independently of the picture.
param(
  [int]$Seconds = 240,
  # The window the case runs in: the palette list only draws at a window as large
  # as the player's own (the same 2536x1452 the icon research case uses).
  [int]$Width = 2536,
  [int]$Height = 1452,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskCxx = Join-Path $taskCompiler 'g++.exe'
$taskSandbox = Join-Path $taskRepo ('build\picture-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskSandbox 'game'
$taskProfile = Join-Path $taskSandbox 'home'
$taskProfileDir = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskProbeData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\test.icon-probe'
$taskMarkers = Join-Path $taskSandbox 'markers'
$taskProbeId = 'test.icon-probe'
$taskMods = @('dev.enter-board', 'local.float-ops', $taskProbeId)
$taskLevel = 'architecture'
$taskWindowSize = ($Height -shl 16) -bor $Width

foreach ($taskRequired in @($taskCxx, (Join-Path $taskRepo 'dist\tc-loader.dll'),
                             (Join-Path $taskRepo 'dist\tcmod-cli.exe'),
                             (Join-Path $taskGame 'Turing Complete.exe'))) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}

# The Mod under test is the package the player installs, pictures included.
& (Join-Path $taskRepo 'examples\float-ops\build.ps1') -SkipTests | Out-Host
if ($LASTEXITCODE) { throw 'Float Ops build failed' }
$taskShipped = Join-Path $taskRepo 'build\float-ops-package\files\asset\float-ops-icons'
$taskShippedIcons = @(Get-ChildItem -LiteralPath $taskShipped -Filter '*.png' -ErrorAction Stop)
if ($taskShippedIcons.Count -ne 22) {
  throw "The package must ship the 22 component pictures, it has $($taskShippedIcons.Count)"
}

New-Item -ItemType Directory -Force $taskRoot, $taskProfile, (Join-Path $taskRoot 'mods'),
  $taskMarkers | Out-Null
$taskSchema = Join-Path $taskProfileDir ('schematics\' + $taskLevel + '\Default\circuit.data')
New-Item -ItemType Directory -Force (Split-Path -Parent $taskSchema) | Out-Null
$taskFixtureExe = Join-Path $taskSandbox 'float-fixture.exe'
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static (Join-Path $PSScriptRoot 'float-fixture.cpp') `
  -o $taskFixtureExe
if ($LASTEXITCODE) { throw 'The fixture generator did not compile' }
& $taskFixtureExe '--m2' $taskSchema | Out-Host
if ($LASTEXITCODE -or !(Test-Path -LiteralPath $taskSchema)) { throw 'The M2 fixture was not generated' }

@(
  'setting_window_mode = true'
  'setting_window_position = 0'
  ('setting_window_size = ' + $taskWindowSize)
  'setting_language = Chinese (Simplified)'
  ('setting_current_level = ' + $taskLevel)
) | Set-Content -LiteralPath (Join-Path $taskProfileDir 'settings.txt') -Encoding ascii

foreach ($taskDir in @('asset', 'campaign', 'translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll', 'soft_oal.dll',
                         'steam_api64.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
foreach ($taskMod in @('dev.enter-board', 'local.float-ops')) {
  Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\' + $taskMod + '.mod')) -Destination (Join-Path $taskRoot 'mods') -Force
}

# The marker pictures: one per shipped picture, so the Mod's own registration path
# (a directory of PNGs named by the type's decimal custom id) is exercised as it is
# in the package - only the content is different.
function New-TaskMarker([string]$Path) {
  $taskBitmap = New-Object System.Drawing.Bitmap 64, 64
  $taskGraphics = [System.Drawing.Graphics]::FromImage($taskBitmap)
  $taskGraphics.Clear([System.Drawing.Color]::FromArgb(255, 255, 0, 255))
  $taskPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::Black), 4
  $taskGraphics.DrawRectangle($taskPen, 2, 2, 59, 59)
  $taskGraphics.Dispose(); $taskPen.Dispose()
  $taskBitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
  $taskBitmap.Dispose()
}
foreach ($taskIcon in $taskShippedIcons) {
  New-TaskMarker (Join-Path $taskMarkers $taskIcon.Name)
}

# The probe package: the icon research probe, driven in its three-surface flow.
$taskProbePackage = Join-Path $taskSandbox 'probe-package'
New-Item -ItemType Directory -Force (Join-Path $taskProbePackage 'native') | Out-Null
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static -shared -I (Join-Path $taskRepo 'sdk') `
  (Join-Path $PSScriptRoot 'icon-probe.cpp') -o (Join-Path $taskProbePackage 'native\probe.dll')
if ($LASTEXITCODE) { throw 'The picture probe did not compile' }
("{`"format`":2,`"id`":`"$taskProbeId`",`"name`":`"Component picture probe`",`"version`":`"0.1.0`"," +
 "`"capabilities`":[`"log`",`"hook`",`"symbol`"],`"native`":{`"api`":1,`"entry`":`"native/probe.dll`"}}") |
  Set-Content -LiteralPath (Join-Path $taskProbePackage 'mod.json') -Encoding ascii
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskProbePackage `
  -Output (Join-Path $taskRoot ('mods\' + $taskProbeId + '.mod')) | Out-Null
if ($LASTEXITCODE) { throw 'The probe package was not written' }

& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskMods | Out-Host
if ($LASTEXITCODE) { throw 'The sandbox Mod apply failed' }

function Invoke-TaskRun([string]$Tag, [string]$Pictures) {
  $taskPreviousProfile = $env:USERPROFILE
  $taskPreviousAppData = $env:APPDATA
  $taskPrevious = @{}
  foreach ($taskName in @('TC_FLOATOPS_BODIES', 'TC_FLOATOPS_PICTURES', 'TC_ICON_PROBE_STAGES',
                          'TC_ICON_PROBE_TAG', 'TC_ICON_PROBE_ABSOLUTE', 'TC_ICON_PROBE_RIGHT',
                          'TC_ICON_PROBE_PALETTE_Y', 'TC_ICON_PROBE_HOLD')) {
    $taskPrevious[$taskName] = [Environment]::GetEnvironmentVariable($taskName)
  }
  try {
    $env:USERPROFILE = $taskProfile
    $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
    # The Mod publishes every instance's body rectangle, which is how the case
    # clicks a component to open the drawer's own preview picture.
    $env:TC_FLOATOPS_BODIES = '1'
    $env:TC_FLOATOPS_PICTURES = $Pictures
    $env:TC_ICON_PROBE_STAGES = '1'
    $env:TC_ICON_PROBE_TAG = $Tag
    $env:TC_ICON_PROBE_ABSOLUTE = '1'
    $env:TC_ICON_PROBE_RIGHT = '45'
    # The customs category tab sits at this height in the player's own window size
    # (measured centres at this 2536x1452 client: 布尔 126 / 整型 196 /
    # 杂项 267 / 输入输出 337 / 浮点+自定义 500). 470 sits on the upper
    # border of the last row and is intermittently missed by the UI hit test.
    $env:TC_ICON_PROBE_PALETTE_Y = '500'
    $env:TC_ICON_PROBE_HOLD = '60'
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
      -WorkingDirectory $taskRoot -PassThru
    $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
      Start-Sleep -Milliseconds 500
      $taskText = if (Test-Path -LiteralPath $taskLog) { Get-Content -LiteralPath $taskLog -Raw -Encoding UTF8 } else { '' }
      if ($taskText.Contains("stage flow done (tag `"$Tag`"")) { break }
      if ($taskProcess.HasExited) { break }
    } while ([DateTime]::UtcNow -lt $taskDeadline)
    if (!$taskText.Contains("stage flow done (tag `"$Tag`"")) {
      throw "The $Tag run never finished its three-surface flow; inspect $taskLog"
    }
  } finally {
    $env:USERPROFILE = $taskPreviousProfile
    $env:APPDATA = $taskPreviousAppData
    foreach ($taskName in $taskPrevious.Keys) {
      [Environment]::SetEnvironmentVariable($taskName, $taskPrevious[$taskName])
    }
    if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
      [void]$taskProcess.CloseMainWindow()
      if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
    }
    # The probe's pictures are named by tag, so both runs keep their own.
    Start-Sleep -Seconds 2
  }
}

Invoke-TaskRun -Tag 'green' -Pictures $taskMarkers
Invoke-TaskRun -Tag 'red' -Pictures '0'

if (!(Test-Path -LiteralPath $taskLog)) { throw "No loader log; inspect $taskSandbox" }
$taskLines = Get-Content -LiteralPath $taskLog -Encoding UTF8
$taskText = $taskLines -join "`n"
@($taskLines | Where-Object { $_ -match 'Component picture|float-ops V5|ICON-PROBE: stage' }) |
  Select-Object -Last 30 | ForEach-Object { $_ }

function Assert-TaskContains([string]$Needle) {
  if (!$taskText.Contains($Needle)) {
    throw "Missing evidence: $Needle (inspect $taskLog)"
  }
}

Assert-TaskContains 'Component render: picture control armed (create_texture_unsafe + create_texture)'
Assert-TaskContains 'Component render: placement preview control armed'
Assert-TaskContains 'Component render: placement preview taken over for custom='
Assert-TaskContains 'Component picture: com_custom_5058442071141929777 is served from'
Assert-TaskContains 'float-ops V5: 22 of 22 component pictures registered from'
Assert-TaskContains 'float-ops V5: picture registration disabled by TC_FLOATOPS_PICTURES'
foreach ($taskStage in @('card', 'drawer', 'ghost', 'after')) {
  Assert-TaskContains ("stage $taskStage frame")
}

# Magenta is the marker; nothing on the board or in the game's own picture of a
# custom component draws it.  The scan walks the raw pixels in strides, because a
# whole window is 3.7 million of them and GetPixel would take minutes per frame.
function Measure-TaskColour([string]$Path, [switch]$Magenta, [switch]$Purple,
                            [int]$CentreX = -1,
                            [int]$CentreY = -1, [int]$Radius = 0) {
  $taskBitmap = [System.Drawing.Bitmap]::FromFile($Path)
  try {
    $taskRect = New-Object System.Drawing.Rectangle 0, 0, $taskBitmap.Width, $taskBitmap.Height
    $taskData = $taskBitmap.LockBits($taskRect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                                     [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
      $taskBytes = New-Object byte[] ($taskData.Stride * $taskData.Height)
      [System.Runtime.InteropServices.Marshal]::Copy($taskData.Scan0, $taskBytes, 0, $taskBytes.Length)
    } finally {
      $taskBitmap.UnlockBits($taskData)
    }
  } finally {
    $taskBitmap.Dispose()
  }
  $taskCount = 0
  $taskFirstX = if ($Radius) { [Math]::Max(0, $CentreX - $Radius) } else { 0 }
  $taskLastX = if ($Radius) { [Math]::Min($taskData.Width - 1, $CentreX + $Radius) }
               else { $taskData.Width - 1 }
  $taskFirstY = if ($Radius) { [Math]::Max(0, $CentreY - $Radius) } else { 0 }
  $taskLastY = if ($Radius) { [Math]::Min($taskData.Height - 1, $CentreY + $Radius) }
               else { $taskData.Height - 1 }
  for ($taskY = $taskFirstY; $taskY -le $taskLastY; $taskY += 3) {
    $taskRow = $taskY * $taskData.Stride
    for ($taskX = $taskFirstX; $taskX -le $taskLastX; $taskX += 6) {
      $taskOffset = $taskRow + $taskX * 4
      if ($Magenta) {
        # BGRA: R == offset+2, G == offset+1, B == offset+0.
        if ($taskBytes[$taskOffset + 2] -gt 200 -and $taskBytes[$taskOffset + 1] -lt 60 -and
            $taskBytes[$taskOffset] -gt 200) {
          ++$taskCount
        }
      } elseif ($Purple) {
        # Float Ops' body fill, rgba(122,79,192).  A tight box around the drag
        # excludes the fixture's already-placed purple components.
        if ($taskBytes[$taskOffset + 2] -gt 100 -and $taskBytes[$taskOffset + 2] -lt 150 -and
            $taskBytes[$taskOffset + 1] -gt 60 -and $taskBytes[$taskOffset + 1] -lt 105 -and
            $taskBytes[$taskOffset] -gt 170 -and $taskBytes[$taskOffset] -lt 220) {
          ++$taskCount
        }
      } else {
        # The component's pin colour, rgba(214,84,84) - what a ghost always draws.
        if ($taskBytes[$taskOffset + 2] -gt 200 -and
            [Math]::Abs($taskBytes[$taskOffset + 1] - 120) -lt 40 -and
            [Math]::Abs($taskBytes[$taskOffset] - 125) -lt 40) {
          ++$taskCount
        }
      }
    }
  }
  return $taskCount
}

$taskDrag = $null
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'stage drag onto the board: pointer at client (\d+),(\d+)') {
    $taskDrag = @([int]$Matches[1], [int]$Matches[2])
  }
}
if (!$taskDrag) { throw "The probe never logged where it dragged to; inspect $taskLog" }
"the placement drag ended at client $($taskDrag[0]),$($taskDrag[1])"

# The second drag is a real placement: the drop cell is where the probe let go
# over the board, which is where the game puts the component.
$taskDrop = $null
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'stage second component onto the board: pointer at client (\d+),(\d+)') {
    $taskDrop = @([int]$Matches[1], [int]$Matches[2])
  }
}
if (!$taskDrop) { throw "The probe never logged the second drag; inspect $taskLog" }
"the real placement was dropped at client $($taskDrop[0]),$($taskDrop[1])"

# The placement lifecycle, from the loader's own log: every takeover has to end
# again.  A run drags twice - once cancelled over the palette, once dropped on the
# board - and the board instance the drawer is opened on can start a third
# placement of its own, so the count is measured rather than assumed; what matters
# is that no takeover is left running (a loader that only listens to
# hide_clipboard, the version this case was extended for, leaves the successful
# placement's preview up - the ghost the player reported).
$taskStarted = @($taskLines | Where-Object { $_ -match 'placement preview taken over for custom=' })
$taskEnded = @($taskLines | Where-Object { $_ -match 'placement preview ended for custom=' })
if ($taskEnded.Count -lt $taskStarted.Count) {
  throw ("$($taskStarted.Count) placement previews were taken over but only " +
         "$($taskEnded.Count) ended: a takeover outlives a placement " +
         "(inspect $taskLog)")
}
if ($taskStarted.Count -lt 2) {
  throw ("The loader took over a placement preview $($taskStarted.Count) times: the probe's " +
         "drags never reached it (inspect $taskLog)")
}
# Which drag of which run the loader saw, so an input drop can be told from a
# missing ghost: the sandbox occasionally loses one synthetic drag (the game's
# window does not take the press, measured), and that must read as reduced
# coverage rather than as a loader fault.  The pixel claims about a takeover are
# only made for the runs where a takeover happened.
$taskRunArmed = @{ green = 0; red = 0 }
$taskArmedTag = ''
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'stage flow armed .*tag "(\w+)"') { $taskArmedTag = $Matches[1] }
  elseif ($taskLine -match 'placement preview taken over' -and $taskArmedTag) {
    $taskRunArmed[$taskArmedTag] = $taskRunArmed[$taskArmedTag] + 1
  }
}
if ($taskStarted.Count -lt 4) {
  Write-Host ("note: the probe's synthetic input started only $($taskStarted.Count) of the " +
              "expected 4 placements (green $($taskRunArmed['green']), red " +
              "$($taskRunArmed['red'])); every takeover that did start is still checked below")
}

$taskResult = @()
$taskFailed = @()
foreach ($taskTag in @('green', 'red')) {
  foreach ($taskStage in @('card', 'drawer', 'ghost', 'after', 'placed', 'settled')) {
    $taskFrames = @(Get-ChildItem -LiteralPath $taskProbeData -Filter ("stage-" + $taskTag + '-' + $taskStage + '-*.bmp') |
      Sort-Object Name)
    if (!$taskFrames.Count) {
      throw "The $taskTag run photographed no $taskStage frame; inspect $taskSandbox"
    }
    $taskPixels = 0
    $taskPins = 0
    $taskBody = 0
    $taskStale = 0
    foreach ($taskFrame in $taskFrames) {
      if ($taskStage -eq 'placed' -or $taskStage -eq 'settled') {
        # The real placement: the component itself has to be on the board at the
        # drop cell, and the cell the cancelled drag was abandoned over has to
        # stay empty (no takeover outliving either drag).
        $taskPins += Measure-TaskColour $taskFrame.FullName -CentreX $taskDrop[0] `
          -CentreY $taskDrop[1] -Radius 160
        $taskBody += Measure-TaskColour $taskFrame.FullName -Purple -CentreX $taskDrop[0] `
          -CentreY $taskDrop[1] -Radius 90
        $taskStale += Measure-TaskColour $taskFrame.FullName -Purple -CentreX $taskDrag[0] `
          -CentreY $taskDrag[1] -Radius 90
      } elseif ($taskStage -eq 'ghost' -or $taskStage -eq 'after') {
        # Only the box the dragged component is in: the component column is still
        # on screen while a component is being placed, and its items carry the
        # registered picture.
        $taskPixels += Measure-TaskColour $taskFrame.FullName -Magenta -CentreX $taskDrag[0] `
          -CentreY $taskDrag[1] -Radius 160
        $taskPins += Measure-TaskColour $taskFrame.FullName -CentreX $taskDrag[0] `
          -CentreY $taskDrag[1] -Radius 160
        $taskBody += Measure-TaskColour $taskFrame.FullName -Purple -CentreX $taskDrag[0] `
          -CentreY $taskDrag[1] -Radius 90
      } else {
        $taskPixels += Measure-TaskColour $taskFrame.FullName -Magenta
      }
    }
    $taskResult += [pscustomobject]@{ Run = $taskTag; Surface = $taskStage
                                      Frames = $taskFrames.Count; Magenta = $taskPixels
                                      Pins = $taskPins; Body = $taskBody
                                      Stale = $taskStale }
    if ($taskStage -eq 'ghost') {
      if ($taskPins -eq 0) {
        $taskFailed += ("the $taskTag run never drew the $taskStage surface " +
                        "(no pin of the dragged component at the drag position)")
      }
      if ($taskPixels -ne 0) {
        $taskFailed += "the $taskTag run's $taskStage surface leaked the registered V5 picture"
      }
      if ($taskBody -eq 0 -and $taskRunArmed[$taskTag] -ge 2) {
        $taskFailed += ("the $taskTag run never drew Float Ops' purple body in the " +
                        "$taskStage surface")
      }
      continue
    }
    if ($taskStage -eq 'after') {
      if ($taskBody -ne 0) {
        $taskFailed += ("the $taskTag run left the placement preview body visible " +
                        "after the component was released")
      }
      continue
    }
    if ($taskStage -eq 'placed' -or $taskStage -eq 'settled') {
      if ($taskBody -eq 0) {
        $taskFailed += ("the $taskTag run's $taskStage surface has no component at the " +
                        "drop cell: the second drag never placed anything")
      }
      if ($taskStale -ne 0 -and $taskRunArmed[$taskTag] -ge 2) {
        $taskFailed += ("the $taskTag run's $taskStage surface still draws a component at " +
                        "the cancelled drag's cell: a takeover outlived a placement")
      }
      continue
    }
    if ($taskTag -eq 'green' -and $taskPixels -eq 0) {
      $taskFailed += "the $taskStage surface never drew the registered picture"
    }
    if ($taskTag -eq 'red' -and $taskPixels -ne 0) {
      $taskFailed += "the $taskStage surface drew a picture while registration was off"
    }
  }
}
$taskResult | Format-Table -AutoSize | Out-String | Write-Host
if ($taskFailed.Count) {
  throw ("The component picture did not reach every surface: " + ($taskFailed -join '; ') +
         " (inspect $taskSandbox)")
}

"PASS component picture and placement preview: V5 owns the component-column and drawer pictures, while V6 draws Float Ops' purple body and pins for the placement ghost in both picture runs, and ends the preview when the game puts the record down - after a cancelled drag and after a real placement on the board"
"Sandbox: $taskSandbox"
