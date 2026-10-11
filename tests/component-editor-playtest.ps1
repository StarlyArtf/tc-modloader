param(
  [ValidateSet('registration','rows')][string]$Stage = 'registration',
  [string]$Row = 'enabled',
  [string]$Keys = '',
  # Which interaction to drive the row with.  '' is the plain click the checkbox,
  # text and hexadecimal cases use; the rest are the V3 shapes - an enum combo, a
  # custom row's context menu, its double click and its resize grip, and the
  # colour widget's sliders.
  [ValidateSet('','click','double','dropdown','menu','grip','wiggle','swatch','select')][string]$Gesture = '',
  # A second row to drive after the first one, and the button that ends a staged
  # table's batch (TC_COMPONENT_EDITOR_TABLE_CONFIRM).  Absent for every case
  # that drives a single row.
  [string]$Row2 = '',
  [ValidateSet('','apply','cancel')][string]$Dialog = '',
  # Rest the pointer on the validation marker after the first gesture, so the
  # message that row is carrying can be read from its tooltip.
  [switch]$Hover,
  # Which choice a dropdown/menu gesture picks (0-based).
  [int]$Choice = 0,
  # Fold the game's own drawer (its title row's switch) after typing, unfold it
  # again, and require the rows to come back in the same place with the typed
  # value still in them - and to draw nothing at all while folded.
  [switch]$Fold,
  # Which of the example Mod's two types to place: '' is the five-field settings
  # type, 'long' is the twenty-row one (the scrolling case).
  [ValidateSet('','long','staged')][string]$Type = ''
)
# Drives tc.component.editor on the real game.
#
#   registration - launches the pinned build in an isolated sandbox with
#                  examples/editor-demo installed and requires the loader to
#                  register the type, accept its field table and say so.  That
#                  covers everything up to the point a player can see the rows:
#                  the service is reachable from a Mod on this build, the field
#                  offsets and kinds pass validation against the type's own
#                  configuration, and the game still comes up.
#
# The row-driving stage (select the instance, click a row, type, commit) needs a
# driver Mod that sends real mouse and keyboard input to the host's window; see
# docs/HANDOFF-component-editor.md for the plan and the assertions it must make.
# Until it runs, docs/verification.md keeps this capability marked unverified.
#
#   rows - runs tests/component-editor-driver.cpp, which enters a level, places
#          one editor-demo component, clicks its body to open the game's own
#          drawer and then clicks the requested row of the editor the loader
#          drew (its rectangles come from the loader's own TC_EDITOR_TRACE log).
#          The assertion is the Mod's own `on_config_changed` line: the click
#          reached the host's row and the host committed through storage.  With
#          -Row label -Keys xy the same run proves text entry (click focuses the
#          field, the characters land in it, Enter commits).
#
#   -Type long runs the same gate against the example's twenty-row type: the
#   loader has to cap its row window at the screen (the trace's own `window` line
#   carries `rows=20` and the scroll range) and the first row has to stay
#   clickable - a capped window that cannot be scrolled would leave the lower
#   rows drawn but unreachable.
#
#   -Gesture drives the V3 shapes through the same driver: `dropdown` opens the
#   enum combo and picks -Choice, `menu` right-clicks a custom row and picks an
#   entry, `double` double-clicks it, `grip` drags its bottom-right handle, and
#   `wiggle` drags across the colour row's sliders.  Each run is retried on a
#   slightly different point until the line it is supposed to produce appears in
#   the loader log (TC_EDITOR_EXPECT), because the popup offsets are ImGui's
#   business rather than the contract's - the same "click again like a player"
#   rule the body click already follows.
#
# Both stages were first made to pass on 2026-09-27; the two traps that cost the
# first attempts are worth keeping in mind when this case is copied:
#   1. `tcmod-cli apply` *replaces* the enabled set.  Two calls leave only the
#      second Mod enabled, and a component whose type was never registered makes
#      PLACE_COMPONENT fail with -6.
#   2. PLACE_COMPONENT also answers -6 when the target cell is already taken, so
#      a driver that places something itself must not use the cell it is about to
#      ask for.
$ErrorActionPreference = 'Stop'
# The game writes loader.log while a run is happening (and may still be flushing
# when it is stopped), so a plain ReadAllText can catch the file mid-write.
function Read-TaskText([string]$Path) {
  for($taskTry = 0; $taskTry -lt 40; $taskTry++) {
    try { return [IO.File]::ReadAllText($Path) } catch [IO.IOException] { Start-Sleep -Milliseconds 250 }
  }
  return [IO.File]::ReadAllText($Path)
}
# The long type's first row is called f0; asking for the settings type's
# "enabled" against it would look for a row that does not exist.
if($Type -eq 'long' -and !$PSBoundParameters.ContainsKey('Row')) { $Row = 'f0' }
# The staged type's two rows: the first is the checkbox the sequence stages, the
# second is the enum it picks through a dropdown.
if($Type -eq 'staged' -and !$PSBoundParameters.ContainsKey('Row')) { $Row = 'active' }
if($Dialog -and !$PSBoundParameters.ContainsKey('Row2')) { $Row2 = 'channel' }
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo

$taskTest = Join-Path $taskRepo ('build\component-editor-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods') | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\local.editor-demo.mod') -Destination (Join-Path $taskRoot 'mods\editor-demo.mod')
if($Stage -eq 'rows') {
  Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\dev.component-editor-driver.mod') -Destination (Join-Path $taskRoot 'mods\editor-driver.mod')
  # The level opener presses the game's own level entry, which is what puts a
  # board (and so a model pointer) in front of the driver.  Everything is enabled
  # in one `apply` call below - a second call replaces the enabled set.
  Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\dev.enter-board.mod') -Destination (Join-Path $taskRoot 'mods\enter-board.mod')
}

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousTrace = $env:TC_EDITOR_TRACE
$taskPreviousLog = $env:TC_EDITOR_LOG
$taskPreviousRow = $env:TC_EDITOR_ROW
$taskPreviousKeys = $env:TC_EDITOR_KEYS
$taskPreviousSkip = $env:TC_EDITOR_SKIP_LEVEL
$taskPreviousGesture = $env:TC_EDITOR_GESTURE
$taskPreviousChoice = $env:TC_EDITOR_CHOICE
$taskPreviousExpect = $env:TC_EDITOR_EXPECT
$taskPreviousItems = $env:TC_EDITOR_ITEMS
$taskPreviousFold = $env:TC_EDITOR_FOLD
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  # One call with every id: `apply` replaces the enabled set, so two calls would
  # leave only the second Mod enabled (measured: the editor-demo package silently
  # disappeared from state.json, the type was never registered, and the placement
  # of its component then failed with TC_COMMAND_ERR_STATE).
  $taskMods = @('local.editor-demo')
  if($Stage -eq 'rows') { $taskMods += @('dev.component-editor-driver', 'dev.enter-board') }
  & (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskMods
  if($LASTEXITCODE){ throw 'The packages could not be enabled' }
  if($Stage -eq 'rows') {
    # The enabled set is what the loader reads at boot; a Mod that is installed
    # but not enabled loads nothing, which is exactly how the first attempt at
    # this case lost the type it was about to place.
    $taskState = Get-Content -LiteralPath (Join-Path $taskRoot 'tc-modloader-data\state.json') -Raw | ConvertFrom-Json
    foreach($taskRequired in $taskMods) {
      if($taskState.enabled -notcontains $taskRequired) { throw "The $taskRequired package is not enabled" }
    }
    $env:TC_EDITOR_TRACE = '1'
    $env:TC_EDITOR_LOG = Join-Path $taskRoot 'tc-modloader-data\loader.log'
    $env:TC_EDITOR_ROW = $Row
    if($Row2) { $env:TC_EDITOR_ROW2 = $Row2 } else { Remove-Item Env:\TC_EDITOR_ROW2 -ErrorAction SilentlyContinue }
    if($Dialog) { $env:TC_EDITOR_DIALOG = $Dialog } else { Remove-Item Env:\TC_EDITOR_DIALOG -ErrorAction SilentlyContinue }
    if($Hover) { $env:TC_EDITOR_HOVER_AFTER = '1' } else { Remove-Item Env:\TC_EDITOR_HOVER_AFTER -ErrorAction SilentlyContinue }
    $env:TC_EDITOR_KEYS = $Keys
    $env:TC_EDITOR_GESTURE = $Gesture
    $env:TC_EDITOR_CHOICE = "$Choice"
    # How many entries the popup has: the driver needs it to aim when ImGui flips
    # a popup above the row (the drawer is at the bottom of the screen).
    if($Gesture -eq 'dropdown') { $env:TC_EDITOR_ITEMS = '5' }
    elseif($Gesture -eq 'menu') { $env:TC_EDITOR_ITEMS = '2' }
    elseif($Row2) { $env:TC_EDITOR_ITEMS = '4' }
    else { Remove-Item Env:\TC_EDITOR_ITEMS -ErrorAction SilentlyContinue }
    if($Fold) { $env:TC_EDITOR_FOLD = '1' } else { Remove-Item Env:\TC_EDITOR_FOLD -ErrorAction SilentlyContinue }
    # What the gesture has to produce before the driver stops retrying.  The
    # refused dropdown choice is the one case whose expected line is a refusal:
    # the validator rejects it and the configuration keeps its old bytes.
    if($Gesture -eq 'dropdown') {
      if($Choice -eq 4) { $env:TC_EDITOR_EXPECT = 'validator refused field "mode"' }
      else { $env:TC_EDITOR_EXPECT = 'committed field "mode"' }
    } elseif($Gesture -eq 'menu') { $env:TC_EDITOR_EXPECT = 'editor-demo: custom action kind=1' }
    elseif($Gesture -eq 'double') { $env:TC_EDITOR_EXPECT = 'editor-demo: custom action kind=2' }
    elseif($Gesture -eq 'grip') { $env:TC_EDITOR_EXPECT = 'editor-demo: custom action kind=5' }
    elseif($Gesture -eq 'wiggle') { $env:TC_EDITOR_EXPECT = 'committed field "color"' }
    elseif($Gesture -eq 'swatch') { $env:TC_EDITOR_EXPECT = 'committed field "palette"' }
    elseif($Row2) {
      # The staged table writes nothing until a button is pressed: Apply is the
      # one write for the whole batch, Cancel is none at all.
      if($Dialog -eq 'cancel') { $env:TC_EDITOR_EXPECT = 'cancelled 2 staged field(s)' }
      else { $env:TC_EDITOR_EXPECT = 'applied 2 staged field(s)' }
    }
    else { Remove-Item Env:\TC_EDITOR_EXPECT -ErrorAction SilentlyContinue }
    if($Type) { $env:TC_EDITOR_TYPE = $Type } else { Remove-Item Env:\TC_EDITOR_TYPE -ErrorAction SilentlyContinue }
  } else {
    Remove-Item Env:\TC_EDITOR_TRACE -ErrorAction SilentlyContinue
    Remove-Item Env:\TC_EDITOR_LOG -ErrorAction SilentlyContinue
    Remove-Item Env:\TC_EDITOR_ROW -ErrorAction SilentlyContinue
    Remove-Item Env:\TC_EDITOR_KEYS -ErrorAction SilentlyContinue
    Remove-Item Env:\TC_EDITOR_GESTURE -ErrorAction SilentlyContinue
    Remove-Item Env:\TC_EDITOR_CHOICE -ErrorAction SilentlyContinue
    Remove-Item Env:\TC_EDITOR_EXPECT -ErrorAction SilentlyContinue
    Remove-Item Env:\TC_EDITOR_ITEMS -ErrorAction SilentlyContinue
    Remove-Item Env:\TC_EDITOR_FOLD -ErrorAction SilentlyContinue
    Remove-Item Env:\TC_EDITOR_TYPE -ErrorAction SilentlyContinue
    Remove-Item Env:\TC_EDITOR_SKIP_LEVEL -ErrorAction SilentlyContinue
  }

  $taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
  if(Test-Path -LiteralPath $taskLog) { Remove-Item -LiteralPath $taskLog -Force }
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  if($Stage -eq 'rows') {
    # The driver writes result.txt when its run finishes, so the wait is for that
    # file rather than for a fixed sleep.
    $taskResult = Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.component-editor-driver\result.txt'
    $taskDeadline = [DateTime]::UtcNow.AddSeconds(180)
    while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
      Start-Sleep -Milliseconds 500
    }
    if(!$taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id -Force }
    $taskText = if(Test-Path -LiteralPath $taskLog) { Read-TaskText $taskLog } else { '' }
    if(!(Test-Path -LiteralPath $taskResult)) {
      throw "The editor driver did not finish; see $taskLog"
    }
    $taskVerdict = [IO.File]::ReadAllText($taskResult)
    if(!$taskVerdict.StartsWith('PASS ')) { throw "The editor driver failed: $taskVerdict" }
    # The type and its field table: without both, the loader never draws a row.
    if($taskText -notmatch 'Component editor V4: type 0x454449545f303031 edits enabled\(bool\), period\(uint\), mode\(enum\), gain\(float\), mask\(uint\), label\(text\), scale\(float\), format\(enum\), color\(color-rgba8\), palette\(color-rgba8\), preview\(custom\)') {
      throw "The field table was not accepted; see $taskLog"
    }
    if($taskText -notmatch 'Component editor V4: type 0x454449545f303033 edits active\(bool\), count\(uint\), channel\(enum\) table=confirm') {
      throw "The staged field table was not accepted with its table flag; see $taskLog"
    }
    # The row the host drew for the selected instance, and the click the driver
    # aimed with it.
    if($taskText -notmatch 'Component editor: row instance=\S+ field="' + [regex]::Escape($Row) + '" rect=[-\d]+,[-\d]+,[-\d]+,[-\d]+') {
      throw "The loader never reported a rectangle for the $Row row; see $taskLog"
    }
    if(!$Gesture -and $taskText -notmatch 'DRIVER: clicked the ' + [regex]::Escape($Row) + ' row') {
      throw "The driver never clicked the $Row row; see $taskLog"
    }
    if($Gesture -and $taskText -notmatch 'DRIVER: ran the ' + [regex]::Escape($Row) + " row's gesture") {
      throw "The driver never ran the $Row row's gesture; see $taskLog"
    }
    # The end-to-end evidence: the host committed the click through
    # tc.component.storage and the type's own on_config_changed reported the new
    # value.  The type's default has enabled=1, so the checkbox click turning it
    # into 0 is a value only that commit can produce.  The refused dropdown
    # choice is one exception (there the absence of a commit *is* the evidence)
    # and a custom row is the other: its own actions write the configuration, so
    # the host never commits a field for it.
    $taskRefusal = $Gesture -eq 'dropdown' -and $Choice -eq 4
    $taskCustomRow = $Row -eq 'preview'
    # A staged table never commits a field on its own: its rows are written by
    # Apply, so the per-field commit line is not what its cases assert.
    $taskStaged = [bool]$Row2
    if(!$taskRefusal -and !$taskCustomRow -and !$taskStaged -and
       $taskText -notmatch 'Component editor: committed field "' + [regex]::Escape($Row) + '"') {
      throw "The host never reported a commit for the $Row row; see $taskLog"
    }
    if($Row -eq 'enabled' -and
       $taskText -notmatch 'editor-demo: instance \S+ config changed enabled=0 period=16 mode=RNE gain=2 mask=0x00000010 label="hello" scale=1') {
      throw "The type's on_config_changed never reported the committed value; see $taskLog"
    }
    # A text row commits what the driver typed into it: the host hands the widget
    # the instance's own bytes, takes the player's characters and writes the whole
    # configuration back on Enter.  The caret sits where the click landed, so the
    # typed text is appended to whatever the field already held ("hello" + "xy" =
    # "helloxy"); what has to hold is that the committed value carries the typing.
    if($Row -eq 'label' -and $Keys -and
       $taskText -notmatch ('editor-demo: instance \S+ config changed enabled=1 period=16 mode=RNE gain=2 mask=0x00000010 label="[^"]*' + [regex]::Escape($Keys) + '[^"]*" scale=1')) {
      throw "The type's on_config_changed never reported the typed text; see $taskLog"
    }
    if($Row -eq 'label' -and $Keys -and
       $taskText -notmatch ('Component editor: committed field "label" on instance \S+ value="?[^"\r\n]*' + [regex]::Escape($Keys))) {
      throw "The host never committed the typed text; see $taskLog"
    }
    # The enum combo (V3): the row opens a dropdown and the choice clicked in the
    # popup is committed.  The example's own validator makes the two outcomes
    # distinguishable - choice 1 only warns, choice 4 is refused outright and the
    # configuration keeps its bytes.
    if($Gesture -eq 'dropdown') {
      if($Choice -eq 4) {
        if($taskText -notmatch 'Component editor: validator refused field "mode" on instance \S+: the demo refuses RNA') {
          throw "The host never reported the validator's refusal for the mode row; see $taskLog"
        }
        if($taskText -match 'Component editor: committed field "mode"') {
          throw "The host committed a mode its own validator had refused; see $taskLog"
        }
      } else {
        $taskMode = @('RNE','RTZ','RDN','RUP','RNA')[$Choice]
        if($taskText -notmatch ('Component editor: committed field "mode" on instance \S+ value=' + $taskMode)) {
          throw "The host never committed the dropdown choice $taskMode; see $taskLog"
        }
        if($taskText -notmatch ('editor-demo: instance \S+ config changed enabled=1 period=16 mode=' + $taskMode + ' gain=2 ')) {
          throw "The type's on_config_changed never reported the dropdown choice; see $taskLog"
        }
        if($Choice -eq 1 -and $taskText -notmatch 'Component editor: validator warned for field "mode" on instance \S+: RTZ is lossy in this demo') {
          throw "The host never reported the validator's warning for the mode row; see $taskLog"
        }
      }
    }
    # The radio form of an enum (V2) still has a row of its own: the loader's
    # rectangle for it is the last choice's button, so a plain click commits that
    # choice.
    if($Row -eq 'format') {
      if($taskText -notmatch 'Component editor: committed field "format" on instance \S+ value=Float') {
        throw "The host never committed the radio row's last choice; see $taskLog"
      }
      if($taskText -notmatch 'editor-demo: instance \S+ config changed .* format=Float') {
        throw "The type's on_config_changed never reported the chosen radio; see $taskLog"
      }
    }
    # The custom row (V3): the Mod draws its own canvas in the host's row and the
    # host routes the interactions to it.
    if($Gesture -eq 'double') {
      if($taskText -notmatch 'editor-demo: custom action kind=2') {
        throw "The host never delivered the custom row's double click; see $taskLog"
      }
    }
    if($Gesture -eq 'menu') {
      if($taskText -notmatch 'editor-demo: custom action kind=1 id=1') {
        throw "The host never delivered the custom row's context menu action; see $taskLog"
      }
      # The menu entry writes configuration through tc.component.storage, so the
      # instance's own on_config_changed reports it (enabled flips off the
      # default 1).
      if($taskText -notmatch 'editor-demo: instance \S+ config changed enabled=0 ') {
        throw "The menu action never reached the configuration; see $taskLog"
      }
    }
    if($Gesture -eq 'grip') {
      foreach($taskKind in @(3,4,5)) {
        if($taskText -notmatch ('editor-demo: custom action kind=' + $taskKind)) {
          throw "The host never delivered resize action kind $taskKind; see $taskLog"
        }
      }
      # The drag's writes reach the configuration, and the host wraps them in one
      # storage transaction (opened on press, committed on release).
      $taskGrip = [regex]::Match($taskText, 'editor-demo: instance \S+ config changed enabled=1 period=16 mode=RNE gain=2 mask=0x00000010 label="hello" scale=1 color=0x[0-9a-f]+ grip=(-?\d+),(-?\d+)')
      if(!$taskGrip.Success -or ($taskGrip.Groups[1].Value -eq '0' -and $taskGrip.Groups[2].Value -eq '0')) {
        throw "The resize grip never reached the configuration; see $taskLog"
      }
    }
    # The colour row (V3): ImGui's own colour widget, whose sliders the driver
    # drags across.  What has to hold is that the stored value really changed.
    if($Gesture -eq 'wiggle') {
      if($taskText -notmatch 'Component editor: committed field "color" on instance \S+ value=#') {
        throw "The host never committed the colour row; see $taskLog"
      }
      $taskChanged = $false
      foreach($taskColor in [regex]::Matches($taskText, 'config changed .*color=0x([0-9a-f]{8})')) {
        if($taskColor.Groups[1].Value -ne 'ff40a0ff') { $taskChanged = $true }
      }
      if(!$taskChanged) {
        throw "The colour drag never changed the stored value; see $taskLog"
      }
    }
    # The hexadecimal widget: what the driver types reaches the configuration.
    # The characters land at the caret the click left behind (editing keys never
    # arrive on this build), so the assertion is that the committed value carries
    # the typed digits - and that the type's own decode agrees with it.  The caret
    # is wherever the *click* put it: the driver clicks the middle of the row, so
    # with a compact field the digits land inside the old value rather than after
    # it ("0xbeef0010" for "beef"), and the value still carries them.
    if($Row -eq 'mask' -and $Keys) {
      $taskHex = $Keys.TrimStart('0x').TrimStart('0X').ToLowerInvariant()
      if($taskText -notmatch ('Component editor: committed field "mask" on instance \S+ value=0x[0-9a-f]*' + [regex]::Escape($taskHex) + '[0-9a-f]*')) {
        throw "The host never committed the typed hexadecimal value; see $taskLog"
      }
      if($taskText -notmatch ('editor-demo: instance \S+ config changed enabled=1 period=16 mode=RNE gain=2 mask=0x[0-9a-f]*' + [regex]::Escape($taskHex) + '[0-9a-f]* label="hello" scale=1')) {
        throw "The type's on_config_changed never reported the typed hexadecimal value; see $taskLog"
      }
    }
    # V4's swatch palette: the strip is what the type declared, and a click on a
    # preset commits that preset's own colour - the packed word the configuration
    # stores, so the type's own decode can agree with the literal.
    if($Gesture -eq 'swatch') {
      if($taskText -notmatch 'Component editor: swatch instance=\S+ field="palette" index=\d+ rect=[-\d]+,[-\d]+,[-\d]+,[-\d]+') {
        throw "The loader never published the palette row's swatches; see $taskLog"
      }
      if($taskText -notmatch 'DRIVER: aiming at swatch ') {
        throw "The driver never aimed at a swatch; see $taskLog"
      }
      $taskSwatchColors = @('ff4040ff','ff30a0ff','ff40e0ff','ff40c040','ffff8040','ffff40a0')
      $taskWanted = $taskSwatchColors[$Choice]
      if($taskText -notmatch 'Component editor: committed field "palette" on instance \S+ value=#') {
        throw "The host never committed the palette row; see $taskLog"
      }
      if($taskText -notmatch ('editor-demo: instance \S+ config changed .*palette=0x' + $taskWanted)) {
        throw "The swatch click never reached the configuration as $taskWanted; see $taskLog"
      }
    }
    # V4's mixed colour row: the editable box beside the strip takes the exact
    # four bytes, the way the hexadecimal UINT row does.  The characters land at
    # the caret the click left behind, so the assertion is that the stored word
    # carries the typed digits.
    if($Row -eq 'palette' -and $Keys) {
      $taskHex = $Keys.TrimStart('0x').TrimStart('0X').ToLowerInvariant()
      if($taskText -notmatch ('Component editor: committed field "palette" on instance \S+ value=#')) {
        throw "The host never committed the colour row's hexadecimal box; see $taskLog"
      }
      if($taskText -notmatch ('editor-demo: instance \S+ config changed .*palette=0x[0-9a-f]*' + [regex]::Escape($taskHex) + '[0-9a-f]*')) {
        throw "The colour row's hexadecimal box never carried the typed digits; see $taskLog"
      }
    }
    # V4's validation marker: the message a row is carrying becomes a square at
    # the end of its control, and the same message is readable from its tooltip.
    if($Hover) {
      if($taskText -notmatch ('Component editor: validation icon instance=\S+ field="' + [regex]::Escape($Row) + '" status=-1 rect=[-\d]+,[-\d]+,[-\d]+,[-\d]+')) {
        throw "The loader never published the validation marker of the $Row row; see $taskLog"
      }
      if($taskText -notmatch ('Component editor: validation tip instance=\S+ field="' + [regex]::Escape($Row) + '" status=-1 message="[^"]+"')) {
        throw "The validation marker never showed its message as a tooltip; see $taskLog"
      }
      if($taskVerdict -notmatch 'tooltip') {
        throw "The driver did not report the marker's tooltip; see $taskLog"
      }
    }
    # V4's staged table (TC_COMPONENT_EDITOR_TABLE_CONFIRM): the rows stage their
    # edits, nothing is written until a button is pressed, and Apply is one write
    # for the whole batch - Cancel is none at all.
    if($taskStaged) {
      foreach($taskField in @($Row,$Row2)) {
        if($taskText -notmatch ('Component editor: staged field "' + [regex]::Escape($taskField) + '"')) {
          throw "The staged rows never reported a staged edit for $taskField; see $taskLog"
        }
        if($taskText -match ('Component editor: committed field "' + [regex]::Escape($taskField) + '"')) {
          throw "A staged table committed $taskField on its own; see $taskLog"
        }
      }
      if($taskText -notmatch 'Component editor: dialog instance=\S+ apply=[-\d]+,[-\d]+,[-\d]+,[-\d]+ cancel=[-\d]+,[-\d]+,[-\d]+,[-\d]+ pending=2') {
        throw "The staged table never published its buttons with both edits waiting; see $taskLog"
      }
      if($taskText -notmatch 'DRIVER: clicked (Apply|Cancel) at') {
        throw "The driver never pressed one of the staged table's buttons; see $taskLog"
      }
      if($Dialog -eq 'cancel') {
        if($taskText -notmatch 'Component editor: cancelled 2 staged field\(s\)') {
          throw "Cancel did not throw the staged edits away; see $taskLog"
        }
        if($taskText -match 'editor-demo: staged instance \S+ config changed') {
          throw "A cancelled batch reached the configuration anyway; see $taskLog"
        }
      } else {
        if($taskText -notmatch 'Component editor: applied 2 staged field\(s\) on instance \S+ in one step') {
          throw "Apply did not write the batch in one step; see $taskLog"
        }
        if($taskText -notmatch 'editor-demo: staged instance \S+ config changed active=0 count=7 channel=') {
          throw "The applied batch never reached the configuration as one change; see $taskLog"
        }
        # One write for the batch means one on_config_changed line: committing the
        # two fields separately would leave two.
        $taskStagedLines = [regex]::Matches($taskText, 'editor-demo: staged instance \S+ config changed').Count
        if($taskStagedLines -ne 1) {
          throw "The applied batch wrote the configuration $taskStagedLines time(s) instead of once; see $taskLog"
        }
      }
    }
    # The scrolling case: the host capped the twenty-row window at the screen and
    # the player can reach the rest, and the first row still commits.
    if($Type -eq 'long') {
      # The first trace line of a freshly drawn window is the frame before the
      # host has measured it, so the assertion looks at the whole run and takes
      # the largest scroll range any frame reported.
      $taskWindows = [regex]::Matches($taskText, 'Component editor: window instance=\S+ pos=[-\d]+,[-\d]+ size=\d+,\d+ rows=32 scrollmax=(\d+)')
      if($taskWindows.Count -eq 0) {
        throw "The loader never reported the long type's row window; see $taskLog"
      }
      $taskScroll = ($taskWindows | ForEach-Object { [int]$_.Groups[1].Value } | Measure-Object -Maximum).Maximum
      if($taskScroll -le 0) {
        throw "The long type's row window never reported a scroll range: a type with more rows than fit would leave its lower rows unreachable; see $taskLog"
      }
      Write-Host ("  row window: rows=32, scroll range up to {0}px" -f $taskScroll)
      if($taskText -notmatch 'editor-demo: long instance \S+ config changed f0=0 f31=1 ones=31') {
        throw "The long type's on_config_changed never reported the committed row; see $taskLog"
      }
    }
    # The editor must live inside the game's own bottom panel (player
    # requirement 2026-09-28).  The loader publishes both rectangles in its
    # window trace, so containment is asserted rather than eyeballed: the last
    # window line is used, because the first frames still carry the unclamped
    # geometry the clamping exists to remove.
    $taskWindows = [regex]::Matches($taskText, 'Component editor: window instance=\S+ pos=(-?\d+),(-?\d+) size=(\d+),(\d+) rows=\d+ scrollmax=\d+ panel=(-?\d+),(-?\d+),(-?\d+),(-?\d+)')
    if($taskWindows.Count -eq 0) {
      throw "The loader never published the drawer geometry of its row window; see $taskLog"
    }
    $taskWindow = $taskWindows[$taskWindows.Count - 1]
    $taskRowX = [int]$taskWindow.Groups[1].Value; $taskRowY = [int]$taskWindow.Groups[2].Value
    $taskRowW = [int]$taskWindow.Groups[3].Value; $taskRowH = [int]$taskWindow.Groups[4].Value
    $taskPanelL = [int]$taskWindow.Groups[5].Value; $taskPanelT = [int]$taskWindow.Groups[6].Value
    $taskPanelR = [int]$taskWindow.Groups[7].Value; $taskPanelB = [int]$taskWindow.Groups[8].Value
    if($taskRowX -lt $taskPanelL -or $taskRowY -lt $taskPanelT -or
       ($taskRowX + $taskRowW) -gt ($taskPanelR + 1) -or
       ($taskRowY + $taskRowH) -gt ($taskPanelB + 1)) {
      throw "The row window leaves the game's bottom panel (window $taskRowX,$taskRowY ${taskRowW}x${taskRowH} in panel $taskPanelL,$taskPanelT..$taskPanelR,$taskPanelB); see $taskLog"
    }
    # The same for the row the driver clicked: it has to sit inside that panel.
    # The last reading of that row, not the first: the rows scroll while the
    # driver brings one into view, and the first line is from before that.
    $taskRowRects = [regex]::Matches($taskText, 'Component editor: row instance=\S+ field="' + [regex]::Escape($Row) + '" rect=(-?\d+),(-?\d+),(-?\d+),(-?\d+)')
    $taskRowRect = if($taskRowRects.Count) { $taskRowRects[$taskRowRects.Count - 1] } else { $null }
    if(!$taskRowRect -or
       [int]$taskRowRect.Groups[1].Value -lt $taskPanelL -or
       [int]$taskRowRect.Groups[2].Value -lt $taskPanelT -or
       [int]$taskRowRect.Groups[3].Value -gt ($taskPanelR + 1) -or
       [int]$taskRowRect.Groups[4].Value -gt ($taskPanelB + 1)) {
      throw "The $Row row leaves the game's bottom panel; see $taskLog"
    }
    # Folding the game's drawer must stop the editor from drawing at all, and
    # unfolding it must bring the same rows back - same rectangle, same typed
    # value still in the field.
    if($Fold) {
      if($taskText -notmatch 'Component editor: the drawer is folded \(') {
        throw "The loader never noticed the drawer was folded; see $taskLog"
      }
      if($taskText -notmatch 'Component editor: the drawer is open again') {
        throw "The loader never noticed the drawer was unfolded; see $taskLog"
      }
      if($taskVerdict -notmatch 'folded with no rows drawn') {
        throw "The driver did not report a clean fold; see $taskLog"
      }
    }
    if($taskText -match 'Component editor: the configuration write for field') {
      throw "The host refused a configuration write; see $taskLog"
    }
    Write-Host "PASS component editor rows: $taskVerdict"
    return
  }
  # The two lines the loader writes while the Mod loads are the whole assertion,
  # so the wait is for them rather than for a fixed sleep.
  $taskRegistered = 'registered custom 0x454449545f303031'
  $taskFields = 'Component editor V4: type 0x454449545f303031 edits enabled\(bool\), period\(uint\), mode\(enum\), gain\(float\), mask\(uint\), label\(text\), scale\(float\), format\(enum\), color\(color-rgba8\), palette\(color-rgba8\), preview\(custom\)'
  $taskDeadline = [DateTime]::UtcNow.AddSeconds(60)
  $taskText = ''
  while([DateTime]::UtcNow -lt $taskDeadline) {
    if(Test-Path -LiteralPath $taskLog) {
      $taskText = Read-TaskText $taskLog
      if($taskText -match $taskFields) { break }
    }
    if($taskProcess.HasExited -and $taskText -notmatch $taskFields) { break }
    Start-Sleep -Milliseconds 500
  }
  if(!$taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id -Force }
  $taskText = Read-TaskText $taskLog
  if($taskText -notmatch $taskRegistered) {
    throw "The editor-demo type was never registered; see $taskLog"
  }
  if($taskText -notmatch $taskFields) {
    throw "The field table was not accepted; see $taskLog"
  }
  # The V4 half of the example: one table declared through the V4 entry point
  # with the table-level CONFIRM flag.  Its acceptance is what says this loader
  # knows the staging shape; the rows case is what proves it works.
  if($taskText -notmatch 'Component editor V4: type 0x454449545f303033 edits active\(bool\), count\(uint\), channel\(enum\) table=confirm') {
    throw "The staged field table was not accepted with its table flag; see $taskLog"
  }
  if($taskText -notmatch 'editor-demo: declared 3 staged editor fields for type 0x454449545f303033') {
    throw "The staged type's field table was never declared; see $taskLog"
  }
  # The description rule (2026-09-28): the game prints a type's description at
  # the top of the bottom drawer, where the property rows live - so the loader
  # keeps it out of the prototype unless the definition opts in.  The example
  # declares one type of each kind, so both lines have to be there.
  if($taskText -notmatch 'Component V2: type 0x454449545f303031 keeps its description out of the game''s panel') {
    throw "The default type's description was not kept out of the game's panel; see $taskLog"
  }
  if($taskText -notmatch 'Component V2: type 0x454449545f303032 shows its description in the game''s panel') {
    throw "The opted-in type's description was not published; see $taskLog"
  }
  if($taskText -match 'plugin load failed|rejected|editor-demo: the field table was refused') {
    throw "The editor-demo plugin did not load cleanly; see $taskLog"
  }
  Write-Host "PASS component editor registration: the type and its field table are accepted by the pinned build"
}
finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:TC_EDITOR_TRACE = $taskPreviousTrace
  $env:TC_EDITOR_LOG = $taskPreviousLog
  $env:TC_EDITOR_ROW = $taskPreviousRow
  $env:TC_EDITOR_KEYS = $taskPreviousKeys
  $env:TC_EDITOR_SKIP_LEVEL = $taskPreviousSkip
  $env:TC_EDITOR_GESTURE = $taskPreviousGesture
  $env:TC_EDITOR_CHOICE = $taskPreviousChoice
  $env:TC_EDITOR_EXPECT = $taskPreviousExpect
  $env:TC_EDITOR_ITEMS = $taskPreviousItems
  $env:TC_EDITOR_FOLD = $taskPreviousFold
}
