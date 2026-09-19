# Circuit-board side panel playtest: registration, real input ownership and
# scene lifetime, driven through the real game window.
#
# What it proves, and how:
#   * the loader registers the panel and draws it inside the board's own
#     window, and never while the main menu is up (loader + plugin log lines);
#   * a real mouse click - WM_LBUTTONDOWN/WM_LBUTTONUP with the cursor placed
#     by the game's own coordinate mapping - reaches the plugin's button;
#   * the game's own input sampling at its board call site agrees that an
#     ImGui item owns the mouse for that frame, which is what keeps the click
#     out of the circuit board.  The driver records the three values the game
#     reads (igIsAnyItemActive / igIsWindowBgActive / igIsWindowHovered) and
#     the pair build_board_ui() hands to handle_io_on_board();
#   * the panel stops being drawn when the level is left (scene lifetime).
#
# Two phases, like the menu page playtest: -Probe records the panel rectangle
# and the button's position inside it into <sandbox>\board-panel-click.txt,
# then the driver run uses that point.  The driver never guesses a position.
param(
  [string]$Sandbox = (Join-Path (Split-Path $PSScriptRoot) 'build\board-panel-sandbox'),
  [int]$Seconds = 45,
  [switch]$Probe,
  # Load the *shipped* example instead of the driver build: it only proves that
  # the example package registers and runs without errors (reaching a board
  # needs the driver, which is the other two modes).
  [switch]$Example,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskRoot = Join-Path $Sandbox 'game'
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskPoints = Join-Path $Sandbox 'board-panel-click.txt'
$taskMod = if ($Example) { 'example.board-panel' } else { 'dev.board-panel-driver' }

# Always refresh the sandbox: make-ui-sandbox.ps1 is what copies the *loader*
# (dist\tc-loader.dll -> game_engine.dll), and a sandbox that keeps an older
# loader silently tests the previous build.  That mistake cost one debugging
# round here: the slot API exists in the loader, so a stale loader means the
# panel never draws at all.
& (Join-Path $PSScriptRoot 'make-ui-sandbox.ps1') -Sandbox $Sandbox -Mods $taskMod
"Loader under test: $((Get-FileHash (Join-Path $taskRoot 'game_engine.dll') -Algorithm SHA256).Hash)"
Remove-Item -LiteralPath $taskLog -ErrorAction SilentlyContinue

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousClick = $env:TC_BOARD_CLICK
$taskProcess = $null
$taskText = ''
try {
  if (!$Probe -and !$Example) {
    if (!(Test-Path -LiteralPath $taskPoints)) {
      throw "Missing $taskPoints; run this playtest with -Probe first"
    }
    $env:TC_BOARD_CLICK = (Get-Content -LiteralPath $taskPoints -Raw).Trim()
    "Driver will click at game coordinates: $($env:TC_BOARD_CLICK)"
  } else {
    $env:TC_BOARD_CLICK = $null
  }
  $env:USERPROFILE = Join-Path $Sandbox 'home'
  $env:APPDATA = Join-Path $Sandbox 'home\AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = (Get-Date).AddSeconds($Seconds)
  $taskStop = if ($Example) {
    'Board panel: registered slot|Native failed|Plugin callback threw|Board panel disabled'
  } elseif ($Probe) {
    'DRIVER: panel button Ping size=|Native failed|Plugin callback threw|Board panel disabled'
  } else {
    'DRIVER: board stopped drawing|DRIVER: giving up|Native failed|Plugin callback threw|Board panel disabled'
  }
  do {
    Start-Sleep -Milliseconds 500
    $taskText = if (Test-Path -LiteralPath $taskLog) { Get-Content -LiteralPath $taskLog -Raw } else { '' }
    if ($taskText -match $taskStop -or $taskProcess.HasExited) { break }
  } while ((Get-Date) -lt $taskDeadline)
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:TC_BOARD_CLICK = $taskPreviousClick
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
if ($taskFatal) { $taskFatal | Select-Object -First 8; throw 'The board panel reported an error' }

if ($Example) {
  # The shipped package: registration and a clean load, nothing more (a board
  # only appears when the driver in the test build enters one).
  foreach ($taskExpected in @(
      'UI slot registered: main (board side panel)',
      'Board panel: registered slot ''main''',
      'Native loaded: example.board-panel')) {
    if (!$taskText.Contains($taskExpected)) {
      $taskLines | Select-Object -Last 20
      throw "Missing evidence for the shipped example: $taskExpected"
    }
  }
  ($taskLines | Where-Object { $_ -match 'UI slot registered|Board panel: registered|Native loaded' }) |
    Select-Object -First 4
  "PASS shipped example.board-panel: loads and registers its board side panel"
  "Sandbox: $Sandbox"
  return
}

if ($Probe) {
  $taskPanel = $taskLines | Where-Object { $_ -match 'Board panel dev\.board-panel-driver/main frame=\d+' } |
    Select-Object -First 1
  $taskOrigin = $taskLines | Where-Object { $_ -match 'panel button Ping origin=(-?\d+),(-?\d+)' } |
    Select-Object -First 1
  $taskSize = $taskLines | Where-Object { $_ -match 'panel button Ping size=(\d+)x(\d+)' } |
    Select-Object -First 1
  $taskSentinel = $taskLines | Where-Object { $_ -match 'clipped probe item local=(-?\d+),(-?\d+) size=' } |
    Select-Object -First 1
  $taskStrip = $taskLines | Where-Object { $_ -match 'scroll strip .* x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)' } |
    Select-Object -First 1
  $taskToggle = $taskLines | Where-Object { $_ -match 'toggle=(-?\d+),(-?\d+) toggleSize=(\d+)x(\d+)' } |
    Select-Object -First 1
  $taskHome = $taskLines | Where-Object { $_ -match 'home button Mods .*window=(\d+)x(\d+)' } |
    Select-Object -First 1
  if (!$taskPanel) { $taskLines | Select-Object -Last 20; throw 'The loader never logged the board panel rectangle' }
  if (!$taskOrigin -or !$taskSize) { throw 'The plugin never reported the button rectangle' }
  if (!$taskSentinel) { throw 'The plugin never reported the clipped probe rectangle' }
  if (!$taskStrip) { throw 'The loader never logged the panel scroll strip rectangle' }
  if (!$taskToggle) { throw 'The loader never logged the panel collapse toggle rectangle' }
  if (!$taskHome) { throw 'No home-page button line with the client size was logged' }
  $null = $taskPanel -match 'frame=\d+ x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) window=(\d+)x(\d+) content=(-?\d+),(-?\d+) size=(\d+)x(\d+)'
  $taskPanelX = [int]$Matches[1]; $taskPanelY = [int]$Matches[2]
  $taskGameW = [int]$Matches[5]; $taskGameH = [int]$Matches[6]
  $taskContentX = [int]$Matches[7]; $taskContentY = [int]$Matches[8]
  $taskContentW = [int]$Matches[9]
  $null = $taskOrigin -match 'origin=(-?\d+),(-?\d+)'
  $taskButtonX = [int]$Matches[1]; $taskButtonY = [int]$Matches[2]
  $null = $taskSize -match 'size=(\d+)x(\d+)'
  $taskButtonW = [int]$Matches[1]; $taskButtonH = [int]$Matches[2]
  $null = $taskSentinel -match 'local=(-?\d+),(-?\d+) size=(\d+)x(\d+)'
  $taskSentinelX = [int]$Matches[1]; $taskSentinelY = [int]$Matches[2]
  $taskSentinelW = [int]$Matches[3]; $taskSentinelH = [int]$Matches[4]
  $null = $taskHome -match 'window=(\d+)x(\d+)'
  $taskClientW = [int]$Matches[1]; $taskClientH = [int]$Matches[2]
  # Both the button and the clipped probe are addressed in *content region*
  # coordinates (the host draws plugin content inside its own child window), so
  # their screen positions are the content origin plus their local position.
  $taskClickX = $taskContentX + $taskButtonX + [int]($taskButtonW / 2)
  $taskClickY = $taskContentY + $taskButtonY + [int]($taskButtonH / 2)
  $taskSentinelScreenX = $taskContentX + $taskSentinelX + [int]($taskSentinelW / 2)
  $taskSentinelScreenY = $taskContentY + $taskSentinelY + [int]($taskSentinelH / 2)
  # The host's scroll strip, from its own logged rectangle: press near its top
  # and drag down.
  $null = $taskStrip -match 'x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)'
  $taskScrollbarX = [int]$Matches[1] + [int]([int]$Matches[3] / 2)
  $taskScrollbarY = [int]$Matches[2] + 24
  $null = $taskToggle -match 'toggle=(-?\d+),(-?\d+) toggleSize=(\d+)x(\d+)'
  $taskToggleX = [int]$Matches[1] + [int]([int]$Matches[3] / 2)
  $taskToggleY = [int]$Matches[2] + [int]([int]$Matches[4] / 2)
  "$taskPanel"
  "$taskOrigin / $taskSize"
  "$taskSentinel"
  $taskReport = 'scale {0} {1} {2} {3};ping {4} {5};sentinel {6} {7};scrollbar {8} {9};collapse {10} {11};canvas 1280 700' -f `
    $taskGameW, $taskGameH, $taskClientW, $taskClientH,
    $taskClickX, $taskClickY, $taskSentinelScreenX, $taskSentinelScreenY,
    $taskScrollbarX, $taskScrollbarY, $taskToggleX, $taskToggleY
  Set-Content -LiteralPath $taskPoints -Value $taskReport -Encoding ascii
  $taskPanelLines = @($taskLines | Where-Object { $_ -match 'Board panel dev\.board-panel-driver/main frame=' })
  "Panel frame log lines while the probe ran: $($taskPanelLines.Count)"
  if ($taskText -match 'UI slot registered: main') { 'Slot registry line present' }
  "PASS board panel probe: rectangle recorded into $taskPoints ($taskReport)"
  "Sandbox: $Sandbox"
  return
}

# Driver run: the evidence the playtest is actually for.
foreach ($taskExpected in @(
    'UI slot registered: main (board side panel)',
    'Board panel: registered slot ''main''',
    'DRIVER: entered the sandbox level',
    'DRIVER: panel first drawn on frame',
    'DRIVER: panel click sent',
    'DRIVER: panel button hovered on frame',
    'DRIVER: panel button click reported count=2',
    'Board panel: Ping clicked, count=3',
    'DRIVER: sentinel click sent',
    'DRIVER: resized the game window from',
    'DRIVER: panel click after resize sent',
    'DRIVER: collapse toggle clicked at',
    'DRIVER: button click while folded sent',
    'DRIVER: expand toggle clicked at',
    'DRIVER: button click after expanding sent',
    'DRIVER: canvas click sent')) {
  if (!$taskText.Contains($taskExpected)) {
    $taskLines | Select-Object -Last 25
    throw "Missing evidence: $taskExpected"
  }
}

# The panel must not exist on the main menu: its first draw has to come after
# the driver left the menu (which its forced home-page press does).  Nothing
# before that line may be a panel frame.
$taskEnteredIndex = ($taskLines | Select-String -Pattern 'DRIVER: pressing a home page entry' |
  Select-Object -First 1).LineNumber
$taskFirstPanelIndex = ($taskLines | Select-String -Pattern 'Board panel dev\.board-panel-driver/main frame=' |
  Select-Object -First 1).LineNumber
if ($taskFirstPanelIndex -lt $taskEnteredIndex) {
  throw 'The board panel drew while the main menu was still up'
}

# Input ownership: while our click is in flight the game's own sampling must
# see an active ImGui item (and no background-active window), which is the
# state build_board_ui() reports as "the board does not own the mouse".
$taskPanelGate = $taskLines | Where-Object { $_ -match 'DRIVER: gate\[panel\]' }
if (!$taskPanelGate) { $taskLines | Select-Object -Last 25; throw 'No gate line was logged for the panel click' }
if (!($taskPanelGate | Where-Object { $_ -match 'busy=1' })) {
  $taskPanelGate | Select-Object -First 8
  throw 'The game never saw an active ImGui item while the panel click was in flight'
}
$taskCanvasGate = $taskLines | Where-Object { $_ -match 'DRIVER: gate\[canvas\]' }
if (!$taskCanvasGate) { $taskLines | Select-Object -Last 25; throw 'No gate line was logged for the canvas click' }
if (!($taskCanvasGate | Where-Object { $_ -match 'busy=0' })) {
  $taskCanvasGate | Select-Object -First 8
  throw 'The canvas click never left the board free to act; the gate was stuck'
}

# Clipping and the panel's own boundary: the item below the fold must be
# unreachable even though the click lands exactly where it would be.  That
# point is *outside* the panel rectangle, so the board legitimately owns it -
# the panel must not claim a rectangle it does not cover.
$taskSentinelGate = $taskLines | Where-Object { $_ -match 'DRIVER: gate\[sentinel\]' }
if (!$taskSentinelGate) { $taskLines | Select-Object -Last 25; throw 'No gate line was logged for the clipped-item click' }
if (!($taskSentinelGate | Where-Object { $_ -match 'busy=0' })) {
  $taskSentinelGate | Select-Object -First 8
  throw 'The panel claimed a click outside its own rectangle'
}
$taskClippedProbe = $taskLines | Where-Object { $_ -match 'clipped probe before scrolling hovers=(\d+) clicks=(\d+)' } |
  Select-Object -First 1
if (!$taskClippedProbe) { throw 'The clipped-item probe never reported its state' }
$null = $taskClippedProbe -match 'hovers=(\d+) clicks=(\d+)'
if ([int]$Matches[1] -ne 0 -or [int]$Matches[2] -ne 0) {
  throw "The item below the fold was reachable before scrolling: $taskClippedProbe"
}

# Scrolling: the host's scroll strip must react to a real drag, and the content
# region's own scroll offset must move (the loader logs that value directly).
$taskStripGrabbed = $taskLines | Where-Object { $_ -match 'scroll strip .* active=1' }
if (!$taskStripGrabbed) {
  ($taskLines | Where-Object { $_ -match 'scroll strip|scrollbar drag' }) | Select-Object -First 8
  throw 'The drag never grabbed the panel scroll strip'
}
$taskScrolled = $null
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'Host content scroll .* y=(-?\d+) max=(\d+)' -and [int]$Matches[1] -gt 0) {
    $taskScrolled = $taskLine
  }
}
if (!$taskScrolled) {
  ($taskLines | Where-Object { $_ -match 'content scroll' }) | Select-Object -First 8
  throw 'The content region never scrolled'
}

# Window size / surface ratio: after the client area changes, the panel is
# re-laid out for the new window, the plugin reports the button's new absolute
# position, and a click there still lands.  That is the automatable half of the
# DPI question - the other half (dragging the window to a monitor with different
# scaling) needs a second monitor.
$taskResizeLine = $taskLines | Where-Object { $_ -match 'DRIVER: resized the game window from (\d+)x(\d+) to (\d+)x(\d+)' } |
  Select-Object -First 1
$null = $taskResizeLine -match 'from (\d+)x(\d+) to (\d+)x(\d+)'
$taskWindowAfter = $Matches[3] + 'x' + $Matches[4]
$taskResizeIndex = ($taskLines | Select-String -Pattern 'DRIVER: resized the game window' |
  Select-Object -First 1).LineNumber
$taskPanelWindows = @()
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'Board panel dev\.board-panel-driver/main .* window=(\d+)x(\d+)') {
    $taskPanelWindows += $Matches[1] + 'x' + $Matches[2]
  }
}
if (!($taskPanelWindows | Where-Object { $_ -eq $taskWindowAfter })) {
  $taskPanelWindows | Select-Object -First 8
  throw "The panel was never laid out for the resized window ($taskWindowAfter)"
}
$taskResized = $taskPanelWindows | Select-Object -Last 1
$taskDisplay = $taskLines | Where-Object { $_ -match 'Display: dpi-awareness=' } | Select-Object -First 1
if (!$taskDisplay) { throw 'The loader never logged the display diagnostics' }
$taskAfterResize = $taskLines | Where-Object { $_ -match 'panel click after resize sent' } | Select-Object -First 1
$null = $taskAfterResize -match 'sent (-?\d+),(-?\d+)'
$taskAfterResizePoint = $Matches[1] + ',' + $Matches[2]
if (!($taskLines | Where-Object { $_ -match 'DRIVER: panel click sent' } |
      Where-Object { $_ -notmatch $taskAfterResizePoint })) {
  throw 'The post-resize click used the same coordinates as the first one'
}
if (!($taskLines | Where-Object { $_ -match 'Board panel: Ping clicked, count=2' })) {
  $taskLines | Select-Object -Last 25
  throw 'The button was not clickable after the window was resized'
}

# Fold / unfold.  The loader owns the toggle, so its own log lines are the
# evidence: collapsed=1 with a header-tall panel, the plugin's button stopping
# to react, and both coming back after expanding.
# On the frame the toggle is clicked the rectangle is still the old one (the
# height for that frame was computed before the state flipped), so the folded
# geometry is the *next* line with collapsed=1.
$taskFoldedHeight = -1
$taskCollapsedLine = $null
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'Board panel dev\.board-panel-driver/main .* h=(\d+) .* collapsed=1') {
    $taskFoldedHeight = [int]$Matches[1]
    $taskCollapsedLine = $taskLine
    if ($taskFoldedHeight -le 200) { break }
  }
}
if (!$taskCollapsedLine) {
  ($taskLines | Where-Object { $_ -match 'Board panel dev|collapse' }) | Select-Object -First 8
  throw 'The panel never reported itself folded'
}
if ($taskFoldedHeight -gt 200) { throw "A folded panel is still $taskFoldedHeight px tall" }
$taskExpandedLine = $taskLines | Where-Object {
  $_ -match 'Board panel dev\.board-panel-driver/main .* h=(\d+) .* collapsed=0' -and
  [int]$Matches[1] -gt 200
} | Select-Object -Last 1
if (!$taskExpandedLine) { throw 'The panel never reported itself unfolded again' }
# While folded, the plugin's button must not have counted the click: the count
# goes 1 (first) -> 2 (after resize) -> 3 (after expanding), never more.
if ($taskLines | Where-Object { $_ -match 'Board panel: Ping clicked, count=4' }) {
  throw 'The button registered a click while the panel was folded'
}
if (!($taskLines | Where-Object { $_ -match 'Board panel: draw count=\d+' })) {
  $taskLines | Select-Object -Last 25
  throw 'The plugin stopped drawing; a folded panel must still call draw()'
}
$taskDrawAfter = $taskLines | Select-String -Pattern 'Board panel: draw count=' | Select-Object -Last 1

# How many frames the panel actually drew, from the driver's own counter (the
# loader only logs the first few rectangle lines).
$taskPanelFrames = 0
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'DRIVER: alive .*panel=(\d+)') {
    $taskPanelFrames = [Math]::Max($taskPanelFrames, [int]$Matches[1])
  }
}
foreach ($taskLine in $taskLines) {
  if ($taskLine -match 'board stopped drawing after the scene change; panel frames=(\d+)') {
    $taskPanelFrames = [Math]::Max($taskPanelFrames, [int]$Matches[1])
  }
}
$taskSceneStop = $taskLines | Where-Object { $_ -match 'DRIVER: board stopped drawing after the scene change' }
# The scene step is a hard assertion again.  It used to be reported as a NOTE
# when the driver's own change_scene(ctx,0) stopped landing: since the loader
# took scene.change over, the driver gets the ctx from the loader's SCENE_CHANGE
# event instead of from its own hook, and the step completes again - so a
# silent degradation here would be a regression, not a flake to live with.
if (!$taskSceneStop) {
  $taskLines | Select-Object -Last 25
  throw 'The panel never stopped drawing after the scene change; the board stayed up'
}
$taskSceneStop | Select-Object -First 1

$taskPanelGate | Select-Object -First 4
$taskCanvasGate | Select-Object -First 4
($taskLines | Where-Object { $_ -match 'DRIVER: panel button|Board panel: Ping clicked' }) | Select-Object -First 4
$taskScrollLine = if ($taskScrolled) { ($taskScrolled -replace '^.*y=', 'y=') } else { '' }
$taskDisplay
$taskResized
$taskAfterResize
$taskCollapsedLine
$taskDrawAfter.Line
"PASS board side panel: registered; drawn $taskPanelFrames frames on the board only; real click "
"delivered to the plugin while the game saw an active item; click below the fold blocked by "
"clipping while a click outside the panel stayed the board's; scroll strip dragged to "
"$taskScrollLine; after resizing the window the button was clicked again at the position the "
"plugin reports; the panel folded to a $taskFoldedHeight px header, its button stopped reacting, "
"and both came back after expanding; canvas click left the board free; panel stopped with the level"
"Sandbox: $Sandbox"
