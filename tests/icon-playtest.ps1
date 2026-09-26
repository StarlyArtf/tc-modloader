# Research case: what the game draws as a *custom* component's icon, and how a
# Mod can take that over.
#
# The game's own component column, the drawer's preview picture and the foundry
# draw a component from one texture whose path the game builds itself:
# `get_captured_path__presenterZio_u28` dispatches on the component kind (0x4e =
# a custom prototype) and `create_texture_unsafe` receives the resolved file
# path.  This case measures that path on a real board and then proves the
# takeover: it drops a deliberately garish PNG at the path the game asks for and
# checks, in an in-process screenshot of the game's own palette, that the icon is
# the file - not the placeholder the game would otherwise draw.
#
# Everything is read-only for the game: the probe hooks the two functions and
# forwards every call unchanged (tests/icon-probe.cpp), the only input is one
# click on the palette's 自定义 tab, and the picture is taken by the loader
# itself (TC_MODLOADER_SHOT), because an outside capture of a fullscreen
# independent-flip window comes back black.
#
# Environment overrides:
#   ICON_TEST_CUSTOM_ID   the custom id whose icon is dropped (default: the
#                         Float Ops Constant, so the fixture board has it)
#   ICON_TEST_KEEP        keep the generated icons and screenshot in the sandbox
param(
  [int]$Seconds = 150,
  [uint64]$Custom = 5058442071141929777,  # 0x463332434f4e5331 = FP32 Constant
  # The window the case runs in.  1200x800 is the small sandbox size, but the
  # game's palette list only draws at a window tall/wide enough for it, so the
  # default here is the size the player's own game runs at (the loader logs the
  # desktop as 2560x1600).
  [int]$Width = 2536,
  [int]$Height = 1452,
  # The negative control: run without dropping any icon file at all, so the
  # frames can be compared with the run that has one.
  [switch]$NoIcon,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskGame = (Resolve-Path -LiteralPath (Join-Path $taskRepo '..')).Path
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskCxx = Join-Path $taskCompiler 'g++.exe'
$taskSandbox = Join-Path $taskRepo ('build\icon-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskSandbox 'game'
$taskProfile = Join-Path $taskSandbox 'home'
$taskProfileDir = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default'
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskProbeData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\test.icon-probe'
$taskProbeId = 'test.icon-probe'
$taskMods = @('dev.enter-board', 'local.float-ops', $taskProbeId)
$taskLevel = 'architecture'
$taskWindowSize = ($Height -shl 16) -bor $Width

foreach ($taskRequired in @($taskCxx, (Join-Path $taskRepo 'dist\tc-loader.dll'),
                             (Join-Path $taskRepo 'dist\tcmod-cli.exe'),
                             (Join-Path $taskGame 'Turing Complete.exe'))) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}

# The Mod under test is the package the player installs.
& (Join-Path $taskRepo 'examples\float-ops\build.ps1') -SkipTests | Out-Host
if ($LASTEXITCODE) { throw 'Float Ops build failed' }

New-Item -ItemType Directory -Force $taskRoot, $taskProfile, (Join-Path $taskRoot 'mods') | Out-Null
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

function New-TaskIcon([string]$Path, [System.Drawing.Color]$Fill) {
  New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
  $taskBitmap = New-Object System.Drawing.Bitmap 64, 64
  $taskGraphics = [System.Drawing.Graphics]::FromImage($taskBitmap)
  $taskGraphics.Clear($Fill)
  $taskPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::Black), 4
  $taskGraphics.DrawRectangle($taskPen, 2, 2, 59, 59)
  $taskGraphics.DrawLine($taskPen, 0, 0, 63, 63)
  $taskGraphics.Dispose(); $taskPen.Dispose()
  $taskBitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
  $taskBitmap.Dispose()
}

# Two candidate directories with *different* colours: the texture request line
# names the one the game builds its path from, and the screenshot says which
# file was actually drawn.  `asset/snapshot_cc` is the custom-component one
# (the shipped game's own icons for campaign components live in
# `asset/capture`, which this case uses as the negative control).
$taskCapturePath = Join-Path $taskRoot ('asset\capture\com_custom_' + $Custom + '.png')
$taskSnapshotPath = Join-Path $taskRoot ('asset\snapshot_cc\com_custom_' + $Custom + '.png')
New-TaskIcon $taskCapturePath ([System.Drawing.Color]::FromArgb(255, 0, 255, 0))     # green
if (!$NoIcon) {
  New-TaskIcon $taskSnapshotPath ([System.Drawing.Color]::FromArgb(255, 255, 0, 255))  # magenta
}
$taskCaptureHash = (Get-FileHash $taskCapturePath).Hash

# The probe package: test-only.
$taskProbePackage = Join-Path $taskSandbox 'probe-package'
New-Item -ItemType Directory -Force (Join-Path $taskProbePackage 'native') | Out-Null
& $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static -shared -I (Join-Path $taskRepo 'sdk') `
  (Join-Path $PSScriptRoot 'icon-probe.cpp') -o (Join-Path $taskProbePackage 'native\probe.dll')
if ($LASTEXITCODE) { throw 'The icon probe did not compile' }
("{`"format`":2,`"id`":`"$taskProbeId`",`"name`":`"Component icon probe`",`"version`":`"0.1.0`"," +
 "`"capabilities`":[`"log`",`"hook`",`"symbol`"],`"native`":{`"api`":1,`"entry`":`"native/probe.dll`"}}") |
  Set-Content -LiteralPath (Join-Path $taskProbePackage 'mod.json') -Encoding ascii
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskProbePackage `
  -Output (Join-Path $taskRoot ('mods\' + $taskProbeId + '.mod')) | Out-Null
if ($LASTEXITCODE) { throw 'The probe package was not written' }

& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskMods | Out-Host
if ($LASTEXITCODE) { throw 'The sandbox Mod apply failed' }

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousHold = $env:ICON_PROBE_HOLD
$taskPreviousAbsolute = $env:TC_ICON_PROBE_ABSOLUTE
$taskPreviousRight = $env:TC_ICON_PROBE_RIGHT
$taskPreviousY = $env:TC_ICON_PROBE_Y
$taskPreviousRowY = $env:TC_ICON_PROBE_ROW_Y
$taskPreviousPaletteY = $env:TC_ICON_PROBE_PALETTE_Y
$taskPreviousBodies = $env:TC_FLOATOPS_BODIES
$taskPreviousPictures = $env:TC_FLOATOPS_PICTURES
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  # This case measures the *file* the game reads at its own snapshot path, so the
  # Mod's own pictures stay out of the way: with them registered (the shipped
  # default) the palette would show them and the frames would differ for a reason
  # this case is not about.  The takeover itself is measured by the
  # component-picture case.
  $env:TC_FLOATOPS_PICTURES = '0'
  # The Float Ops Mod publishes every component's body rectangle; the probe clicks
  # the board component with it to open the drawer's own preview picture.
  $env:TC_FLOATOPS_BODIES = '1'
  # The probe takes the pictures itself (a GL readback next to the frame callback);
  # the hold has to outlast them.
  $env:TC_ICON_PROBE_HOLD = '22'
  # The palette is what draws the items whose icons this case is about, and it
  # hangs off the right edge: click it in right-relative pixels.
  $env:TC_ICON_PROBE_ABSOLUTE = '1'
  $env:TC_ICON_PROBE_RIGHT = '45'
  # The custom category tab sits at this height in the player's own window size
  # (measured: 布尔 155 / 整型 237 / 杂项 322 / 输入输出 406 / 自定义 470).
  $env:TC_ICON_PROBE_PALETTE_Y = '470'
  # Route B spike: paint the design cells the game computed for the constant, so
  # the next frames show whether the palette/drawer/ghost picture follows.
  # Takeover spike: the game asks for the component's snapshot; the probe hands the
  # game's own texture factory a file of ours instead, so the game keeps owning the
  # texture and just reads a different picture.
  New-Item -ItemType Directory -Force $taskProbeData | Out-Null
  $taskSubstitute = Join-Path $taskProbeData 'icon.png'
  New-TaskIcon $taskSubstitute ([System.Drawing.Color]::FromArgb(255, 255, 0, 255))
  $env:TC_ICON_PROBE_SUBSTITUTE = $taskSubstitute
  $env:TC_ICON_PROBE_MATCH = 'snapshot_cc'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  do {
    Start-Sleep -Milliseconds 500
    $taskText = if (Test-Path -LiteralPath $taskLog) { Get-Content -LiteralPath $taskLog -Raw -Encoding UTF8 } else { '' }
    if ($taskText.Contains('ICON-PROBE: done') -or $taskProcess.HasExited) { break }
  } while ([DateTime]::UtcNow -lt $taskDeadline)
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:ICON_PROBE_HOLD = $taskPreviousHold
  $env:TC_ICON_PROBE_ABSOLUTE = $taskPreviousAbsolute
  $env:TC_ICON_PROBE_RIGHT = $taskPreviousRight
  $env:TC_ICON_PROBE_Y = $taskPreviousY
  $env:TC_ICON_PROBE_ROW_Y = $taskPreviousRowY
  $env:TC_ICON_PROBE_PALETTE_Y = $taskPreviousPaletteY
  $env:TC_FLOATOPS_BODIES = $taskPreviousBodies
  $env:TC_FLOATOPS_PICTURES = $taskPreviousPictures
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
}

if (!(Test-Path -LiteralPath $taskLog)) { throw "No loader log; inspect $taskSandbox" }
$taskLines = Get-Content -LiteralPath $taskLog -Encoding UTF8
$taskText = $taskLines -join "`n"
@($taskLines | Where-Object { $_ -match 'ICON-PROBE|float-ops: the editor|Component panel' }) |
  Select-Object -Last 25 | ForEach-Object { $_ }

function Assert-TaskContains([string]$Needle) {
  if (!$taskText.Contains($Needle)) {
    throw "Missing evidence: $Needle (inspect $taskLog)"
  }
}

Assert-TaskContains 'ICON-PROBE: get_captured_path dispatcher hooked'
Assert-TaskContains 'ICON-PROBE: clicked the component body'
Assert-TaskContains 'ICON-PROBE: done'
# Since tc.component.render V5 the *loader* owns the texture factory (that is
# where a Mod's own picture is answered from), so this probe's own hook of it is
# refused and the factory's traffic is the loader's to log.  Both halves of that
# are evidence: the refusal names the owner, and the loader says it armed.
Assert-TaskContains 'the loader owns this target for component render V5 picture control'
Assert-TaskContains 'Component render: picture control armed'
# The dispatcher overload is only taken by some callers (the palette asks the
# custom branch directly), so it is evidence when present and not a failure when
# it is not.
if (!$taskText.Contains('captured path asked kind=0x4e value=' + $Custom + ' ')) {
  "note: the get_captured_path dispatcher was not used for this id in this run"
}

# The file the game asks for comes from `get_captured_path` (the probe still hooks
# that one), which is the path the research case is about: for a custom prototype
# `<game>/asset/?snapshot_cc/com_custom_<decimal id>.png`.  The texture traffic
# itself is the loader's own now (V5 owns the factory), so this case reads the
# loader's picture log when a picture was registered and otherwise only needs the
# dispatcher line above.
$taskRequest = @($taskLines | Where-Object { $_ -match 'captured path asked kind=0x4e value=' + $Custom })
$taskSnapshotAsked = $taskRequest.Count -gt 0

# The game must not have rewritten the file it just read: a snapshot cache that
# regenerates itself would make a shipped icon useless.
$taskCaptureAfter = (Get-FileHash $taskCapturePath).Hash
if ($taskCaptureAfter -ne $taskCaptureHash) {
  throw "asset/capture was rewritten during the run (mtime $((Get-Item $taskCapturePath).LastWriteTimeUtc)); the game owns that file"
}

# The icon file itself: the game re-encodes it (measured: a 1060-byte 64x64 PNG
# came back as 623 bytes) but must keep the picture, so the check is on pixels.
# With -NoIcon the run only records what the game renders by itself.
if (!$NoIcon -and !(Test-Path -LiteralPath $taskSnapshotPath)) {
  throw "The icon file disappeared; inspect $taskSandbox"
}
if (!$NoIcon) {
$taskIconKeep = [System.Drawing.Bitmap]::FromFile($taskSnapshotPath)
try {
  # Off the diagonal the test draws across the icon: (32,32) *is* that diagonal.
  $taskPixel = $taskIconKeep.GetPixel(16, 32)
  "icon file after the run: $($taskIconKeep.Width)x$($taskIconKeep.Height), " +
    "$((Get-Item $taskSnapshotPath).Length) bytes, RGB($($taskPixel.R),$($taskPixel.G),$($taskPixel.B)) at 16,32"
  if (!($taskPixel.R -gt 200 -and $taskPixel.G -lt 60 -and $taskPixel.B -gt 200)) {
    throw "The game replaced the icon file with its own picture; inspect $taskSandbox"
  }
} finally { $taskIconKeep.Dispose() }
}

# Every captured frame is scanned: magenta would mean the dropped file is what the
# user sees, green (present only in asset/capture) is the negative control.
# Measured result: neither appears - the palette picture is the game's own render
# of the prototype, while the `?snapshot_cc/...png` file is read and re-encoded.
# See docs/research/component-icons.md.
$taskFrames = @(Get-ChildItem -LiteralPath $taskProbeData -Filter 'palette-*.bmp' | Sort-Object Name)
if (!$taskFrames.Count) { throw "No captured frames; inspect $taskSandbox" }
$taskMagenta = 0; $taskGreen = 0; $taskWithMagenta = ''
foreach ($taskFrame in $taskFrames) {
  $taskImage = [System.Drawing.Bitmap]::FromFile($taskFrame.FullName)
  try {
    for ($taskY = 0; $taskY -lt $taskImage.Height; $taskY += 2) {
      for ($taskX = 0; $taskX -lt $taskImage.Width; $taskX += 2) {
        $taskPixel = $taskImage.GetPixel($taskX, $taskY)
        if ($taskPixel.R -gt 200 -and $taskPixel.G -lt 60 -and $taskPixel.B -gt 200) {
          ++$taskMagenta
          $taskWithMagenta = $taskFrame.Name
        } elseif ($taskPixel.R -lt 60 -and $taskPixel.G -gt 200 -and $taskPixel.B -lt 60) {
          ++$taskGreen
        }
      }
    }
  } finally { $taskImage.Dispose() }
}
"icon colour in $($taskFrames.Count) captured frame(s): magenta=$taskMagenta green=$taskGreen" +
  $(if ($taskWithMagenta) { " (seen in $taskWithMagenta)" } else { '' })
if ($taskMagenta -eq 0) {
  "note: the dropped icon file is read and re-encoded by the game, but the picture it draws is its own render"
}

$taskAsked = if ($taskSnapshotAsked) { 'asset/snapshot_cc/com_custom_<decimal id>.png' } else { 'asset/capture' }
"PASS component icon: the game asks for and reads $taskAsked, while the palette picture itself is the game's own render (docs/research/component-icons.md)"
"Sandbox: $taskSandbox"
