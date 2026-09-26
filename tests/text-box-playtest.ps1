# End-to-end check of the text-box component: build the Mod, run it in an
# isolated copy of the game beside the "enter a board" driver, let the Mod's own
# self-test place four rotated notes, verify live zoom and pan transforms, edit
# one note and read the configuration back, prove the note whose game drawing is
# turned off can still be grabbed and dragged over its declared footprint, and
# take the picture from inside the process (the sandbox window is not visible
# from outside the process).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests/text-box-playtest.ps1
#
# Products: build/text-box-out/board.png (the board with the note) and the
# loader log lines the run asserts on.
param(
  [int]$Seconds = 70,
  [int]$ShotDelay = 24000,
  [switch]$KeepRunning
)
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskGame=Split-Path $taskRepo
$taskSandbox=Join-Path $taskRepo 'build\text-box-sandbox'
$taskRoot=Join-Path $taskSandbox 'game'
$taskOut=Join-Path $taskRepo 'build\text-box-out'

& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $taskRepo 'build-text-box.ps1')
if ($LASTEXITCODE) { throw 'Text box build failed' }

if (Test-Path -LiteralPath $taskSandbox) {
    $resolved=(Resolve-Path -LiteralPath $taskSandbox).Path
    if (-not $resolved.StartsWith((Resolve-Path -LiteralPath (Join-Path $taskRepo 'build')).Path)) {
        throw "Refusing to remove a sandbox outside build/: $taskSandbox"
    }
    Remove-Item -LiteralPath $taskSandbox -Recurse -Force
}
New-Item -ItemType Directory -Force $taskRoot,(Join-Path $taskSandbox 'home'),$taskOut | Out-Null
foreach ($taskDir in @('asset','campaign','translations')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot -Force
}
New-Item -ItemType Directory -Force (Join-Path $taskRoot 'mods') | Out-Null
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
foreach ($taskMod in @('local.text-box','dev.enter-board')) {
    Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\'+$taskMod+'.mod')) -Destination (Join-Path $taskRoot 'mods') -Force
}
# The live install's asset files may already carry another Mod's patch; the
# sandbox starts from the loader's own copies of the originals so that applying
# a Mod here can never patch an already-patched file twice.
$taskStatePath=Join-Path $taskGame 'tc-modloader-data\state.json'
if (Test-Path -LiteralPath $taskStatePath) {
    $taskState=Get-Content -LiteralPath $taskStatePath -Raw | ConvertFrom-Json
    foreach ($taskFile in $taskState.files.PSObject.Properties) {
        $taskBlob=Join-Path $taskGame ('tc-modloader-data\blobs\'+$taskFile.Value.original)
        if (Test-Path -LiteralPath $taskBlob) {
            Copy-Item -LiteralPath $taskBlob -Destination (Join-Path $taskRoot $taskFile.Name) -Force
        }
    }
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply local.text-box dev.enter-board
if ($LASTEXITCODE) { throw 'Sandbox mod apply failed' }
$taskData=Join-Path $taskRoot 'tc-modloader-data\plugin-data\local.text-box'
New-Item -ItemType Directory -Force $taskData | Out-Null
Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value 'sandbox' -Encoding ascii

$taskLog=Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskShot=Join-Path $taskOut 'board.bmp'
Remove-Item -LiteralPath $taskLog,$taskShot -ErrorAction SilentlyContinue
$taskPreviousProfile=$env:USERPROFILE;$taskPreviousAppData=$env:APPDATA
$taskPreviousShot=$env:TC_MODLOADER_SHOT;$taskPreviousDelay=$env:TC_MODLOADER_SHOT_DELAY
$taskPreviousRenderClip=$env:TC_MODLOADER_RENDER_CLIP
$taskPreviousDefaultProbe=$env:TC_TEXTBOX_DEFAULT_PROBE
$taskProcess=$null
try {
    $env:USERPROFILE=Join-Path $taskSandbox 'home'
    $env:APPDATA=Join-Path $env:USERPROFILE 'AppData\Roaming'
    $env:TC_MODLOADER_SHOT=$taskShot
    $env:TC_MODLOADER_SHOT_DELAY="$ShotDelay"
    # Inset all four edges over clear board pixels. The Mod draws a different
    # opaque marker across each edge and this script checks the captured pixels.
    $env:TC_MODLOADER_RENDER_CLIP='800,200,1800,850'
    $env:TC_TEXTBOX_DEFAULT_PROBE='1'
    # The arc scenario also checks that the note's whole layout (box, edit button,
    # text, padding) stays the same in board cells at every zoom it passes through.
    if($env:TC_TEXTBOX_KEEP_SELECTION -and $env:TC_TEXTBOX_KEEP_SELECTION -ne '0'){
        $env:TC_TEXTBOX_LAYOUT_TRACE='1'
        # The foundry button's own rect only exists while the panel draws it, so the
        # trace that reports it is on in the scenarios that can measure it.
        $env:TC_FOUNDRY_TRACE='1'
    }
    $taskProcess=Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline=(Get-Date).AddSeconds($Seconds)
    do {
        Start-Sleep -Milliseconds 500
        $taskText=if(Test-Path -LiteralPath $taskLog){Get-Content -LiteralPath $taskLog -Raw}else{''}
        if($taskText -match 'text-box autotest: the click' -and
           $taskText -match 'Captured frame screenshot' -or
           $taskText -match 'Captured frame screenshot' -or
           $taskText -match 'Plugin callback threw|Native failed' -or
           $taskProcess.HasExited){break}
    }while((Get-Date)-lt $taskDeadline)
}finally{
    $env:USERPROFILE=$taskPreviousProfile;$env:APPDATA=$taskPreviousAppData
    $env:TC_MODLOADER_SHOT=$taskPreviousShot;$env:TC_MODLOADER_SHOT_DELAY=$taskPreviousDelay
    $env:TC_MODLOADER_RENDER_CLIP=$taskPreviousRenderClip
    $env:TC_TEXTBOX_DEFAULT_PROBE=$taskPreviousDefaultProbe
    if($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning){
        [void]$taskProcess.CloseMainWindow()
        if(!$taskProcess.WaitForExit(4000)){Stop-Process -Id $taskProcess.Id}
    }
}
if(!(Test-Path -LiteralPath $taskLog)){throw 'No loader log: the game did not start'}
$taskText=Get-Content -LiteralPath $taskLog -Raw
if($taskText -match 'Plugin callback threw|Component render callback threw|Native failed'){
    throw 'The text-box or component render callback failed in the sandbox'
}
# The selection-hint scenario (TC_TEXTBOX_KEEP_SELECTION=1) keeps the dragged note
# selected so the captured frame can be measured for the game's own arc; it has no
# deselect line, and it reports the footprint window of two instances instead.
$taskKeepSelection=($env:TC_TEXTBOX_KEEP_SELECTION -and $env:TC_TEXTBOX_KEEP_SELECTION -ne '0')
# TC_TEXTBOX_KEEP_ARCS=1 leaves the game's own selection hint on for the note; it is
# the sensitivity control for every arc measurement below.
$taskKeepArcs=($env:TC_TEXTBOX_KEEP_ARCS -and $env:TC_TEXTBOX_KEEP_ARCS -ne '0')
foreach($taskNeedle in @(
    'Component render: overlay primitives armed',
    'Component render: default drawing control armed',
    'Component render: selection hint control armed',
    'Component geometry V2: native instance-footprint hit testing armed',
    'text-box: type footprint registered half=4.0,2.0; live instances follow rendered box size',
    'text-box: game default drawing disabled for note type',
    'text-box: tc.component.render callback active',
    'PASS text-box render transforms rotations=0xf',
    'PASS text-box render zoom live unit',
    'PASS text-box render pan delta=',
    'Component render: diagnostic clip 800.000000,200.000000..1800.000000,850.000000',
    'text-box autotest: clip pixel markers armed rect=800,200..1800,850',
    'PASS text-box default drawing probe centers hidden=',
    'Component render: suppressed game default drawing custom=',
    'PASS text-box hidden note drag from ',
    'PASS text-box native instance footprint selected outside type footprint by=',
    'PASS text-box selection hint instance=',
    'text-box autotest: the click opened the editor',
    'Captured frame screenshot'
)){
    if(-not $taskText.Contains($taskNeedle)){throw "Missing text-box render evidence: $taskNeedle"}
}
# The node gesture has two modes and exactly one of them ran.  In the default run
# the note's resize nodes own the board mouse for the whole hover/press/drag/release
# lifecycle: the width and height change, the note does not move, and the board's own
# input sample was answered "an item is active" - with the board's object counts
# (components, wires) unchanged across the gesture, which is the number behind the
# report "dragging a node also drew a wire".  TC_TEXTBOX_BLOCK_BOARD_INPUT=0 is the
# sensitivity control for that: the same synthetic gesture with the block off, which
# has to reach the board (the note moves and/or the board gains an object).  Both
# runs use the same code and the same synthetic mouse events.
$taskBlockOff=($env:TC_TEXTBOX_BLOCK_BOARD_INPUT -eq '0')
if($taskBlockOff){
    foreach($taskNeedle in @(
        'text-box: resize input block is OFF (control run)',
        'PASS text-box resize control: without the block the same drag reached the board'
    )){
        if(-not $taskText.Contains($taskNeedle)){throw "Missing text-box control evidence: $taskNeedle"}
    }
    $taskControl=[regex]::Match($taskText,
        'text-box resize control: board-input block off moved=([0-9]+) components=([-0-9]+) wires=([-0-9]+) input-blocks=([0-9]+) resized=([0-9]+)')
    if(-not $taskControl.Success){throw 'The resize control line was not parseable'}
    "PASS text-box resize control: the board took the gesture moved=$($taskControl.Groups[1].Value) components=$($taskControl.Groups[2].Value) wires=$($taskControl.Groups[3].Value) input-blocks=$($taskControl.Groups[4].Value) resized=$($taskControl.Groups[5].Value)"
}else{
    foreach($taskNeedle in @(
        'text-box: resize handles own the board mouse while hovered or dragged',
        'text-box: board mouse-state hooks armed=4/4',
        'PASS text-box resize handles width=',
        'PASS text-box text layout follows resized top edge',
        'PASS text-box resize handles captured board input samples='
    )){
        if(-not $taskText.Contains($taskNeedle)){throw "Missing text-box render evidence: $taskNeedle"}
    }
    $taskResize=[regex]::Match($taskText,
        'PASS text-box resize handles captured board input samples=([0-9]+) board-objects=unchanged mouse-reads-hidden=([0-9]+)')
    if(-not $taskResize.Success){throw 'The resize input-block line was not parseable'}
    if([int]$taskResize.Groups[1].Value -lt 1){throw 'The node gesture never reached the board sample site'}
    # Both halves matter: the sample answer ("an item is active") steers the board's
    # branch, and the mouse-state hooks are what actually keep the press, the drag and
    # the release out of the board's own input state.  Measured live, the wire the
    # report was about survived the first half alone.
    if([int]$taskResize.Groups[2].Value -lt 1){throw 'The board never read the hidden mouse state: the hooks did not cover the board'}
    "PASS text-box resize input ownership blocked $($taskResize.Groups[1].Value) board input sample(s) and hid $($taskResize.Groups[2].Value) mouse-state read(s), board objects unchanged"
}
# With the arcs left on (the sensitivity control) the Mod never asks the host to
# drop the game's hint, so the suppression line is expected to be absent there.
if(-not $taskKeepArcs){
    $taskRingNeedle='Component render: cleared the game''s selection ring for '
    if(-not $taskText.Contains($taskRingNeedle)){
        throw "Missing text-box render evidence: $taskRingNeedle"
    }
}
# The foundry button line follows the same rule: the control run asks the host to
# keep it (TC_TEXTBOX_FOUNDRY_BUTTON=1) and reports that instead.
$taskFoundryKeptRun=($env:TC_TEXTBOX_FOUNDRY_BUTTON -and $env:TC_TEXTBOX_FOUNDRY_BUTTON -ne '0')
$taskFoundryNeedle=if($taskFoundryKeptRun){
    'text-box: keeping the game''s foundry edit button as the control'
}else{
    'text-box: the game''s foundry edit button is off for the note type'
}
if(-not $taskText.Contains($taskFoundryNeedle)){
    throw "Missing text-box render evidence: $taskFoundryNeedle"
}
if($taskKeepSelection){
    # Nothing else to require here: the arc window itself is asserted on the
    # captured pixels below, and the frame is only usable because the note is still
    # the selected instance when it is taken (checked there too).
    # "The note follows the camera" as a number: TC_TEXTBOX_LAYOUT_TRACE=1 reports the
    # layout in board cells once a second, so every distinct zoom in the log has to
    # report the same box, button, text and padding - that is exactly what the
    # fixed-pixel floors used to break.
    $taskLayouts=@{}
    foreach($taskLine in (Get-Content -LiteralPath $taskLog |
            Select-String 'text-box layout:' | ForEach-Object { $_.Line })){
        $taskLayout=[regex]::Match($taskLine,
            'unit=([-0-9.]+) box=([-0-9.]+)x([-0-9.]+) cells button=([-0-9.]+) cells text=([-0-9.]+) cells padding=([-0-9.]+) cells')
        if(!$taskLayout.Success){continue}
        $taskLayouts[$taskLayout.Groups[1].Value]=@(
            [double]$taskLayout.Groups[2].Value,[double]$taskLayout.Groups[3].Value,
            [double]$taskLayout.Groups[4].Value,[double]$taskLayout.Groups[5].Value,
            [double]$taskLayout.Groups[6].Value)
    }
    if($taskLayouts.Count -ge 2){
        $taskReferenceUnit=($taskLayouts.Keys | Sort-Object {[double]$_})[0]
        $taskReference=$taskLayouts[$taskReferenceUnit]
        foreach($taskUnitKey in $taskLayouts.Keys){
            $taskValues=$taskLayouts[$taskUnitKey]
            for($taskIndex=0;$taskIndex -lt 5;$taskIndex++){
                if([Math]::Abs($taskValues[$taskIndex]-$taskReference[$taskIndex]) -gt 0.05){
                    throw "The note layout changes with the camera: unit=$taskUnitKey reports $($taskValues -join ',') against $($taskReference -join ',') at unit=$taskReferenceUnit"
                }
            }
        }
        "PASS text-box note layout stays $(($taskReference | ForEach-Object {[Math]::Round($_,2)}) -join 'x') cells at $($taskLayouts.Count) camera scales"
    }
    # V4: the game's "edit this component in the foundry" button on the component
    # panel is dropped for this Mod's types.  The trace reports the button's own rect
    # while it is submitted, so one picture can prove both halves: nothing painted
    # here, and hundreds of pixels at exactly that rect in the control run
    # (TC_TEXTBOX_FOUNDRY_BUTTON=1 keeps the game's button).
    $taskFoundryLines=@(Get-Content -LiteralPath $taskLog |
        Select-String 'foundry button box=' | ForEach-Object { $_.Line })
    if($taskFoundryLines.Count -eq 0){throw 'The foundry button trace reported no rect'}
    $taskFoundry=[regex]::Match($taskFoundryLines[-1],
        'box=([-0-9.]+),([-0-9.]+)\.\.([-0-9.]+),([-0-9.]+) hidden=([0-9]+) selected=([0-9]+)')
    if(-not $taskFoundry.Success){throw "The foundry button trace was not parseable: $($taskFoundryLines[-1])"}
    if([int]$taskFoundry.Groups[6].Value -lt 1){throw 'The foundry button was traced with nothing selected'}
    $taskFoundryHidden=([int]$taskFoundry.Groups[5].Value -eq 1)
    $taskFoundryKept=($env:TC_TEXTBOX_FOUNDRY_BUTTON -and $env:TC_TEXTBOX_FOUNDRY_BUTTON -ne '0')
    if($taskFoundryKept -eq $taskFoundryHidden){
        throw "The foundry button trace disagrees with the run: hidden=$($taskFoundry.Groups[5].Value) kept=$taskFoundryKept"
    }
}elseif(-not $taskText.Contains('PASS text-box hidden note deselected selection=0')){
    throw 'Missing text-box render evidence: PASS text-box hidden note deselected selection=0'
}
$taskDragMatch=[regex]::Match(
    $taskText,
    'PASS text-box hidden note drag from (-?[0-9]+),(-?[0-9]+) to (-?[0-9]+),(-?[0-9]+) selection=([0-9]+)'
)
if(-not $taskDragMatch.Success){throw 'The hidden-note drag line was not parseable'}
$taskDragFromX=[int]$taskDragMatch.Groups[1].Value
$taskDragFromY=[int]$taskDragMatch.Groups[2].Value
$taskDragToX=[int]$taskDragMatch.Groups[3].Value
$taskDragToY=[int]$taskDragMatch.Groups[4].Value
if($taskDragToX -ne $taskDragFromX+1 -or $taskDragToY -ne $taskDragFromY){
    throw "The hidden note did not move by one board cell: $($taskDragMatch.Value)"
}
"PASS text-box hidden note drag $taskDragFromX,$taskDragFromY -> $taskDragToX,$taskDragToY selection=$($taskDragMatch.Groups[5].Value)"
# The Mod's own selection hint has to follow the declared 8x4 footprint, not the
# game's component mesh: the line reports the screen box it drew plus the number
# of pixels one board unit covers in the same frame.
$taskHintMatch=[regex]::Match(
    $taskText,
    'PASS text-box selection hint instance=[0-9]+ centre=([-0-9.]+),([-0-9.]+) half=4\.00,2\.00 box=([-0-9.]+),([-0-9.]+)\.\.([-0-9.]+),([-0-9.]+) unit=([-0-9.]+) rotation=([0-3])'
)
if(-not $taskHintMatch.Success){throw 'The selection-hint line was not parseable'}
$taskHintCentreX=[double]::Parse($taskHintMatch.Groups[1].Value,$taskInvariant)
$taskHintCentreY=[double]::Parse($taskHintMatch.Groups[2].Value,$taskInvariant)
$taskHintMinX=[double]::Parse($taskHintMatch.Groups[3].Value,$taskInvariant)
$taskHintMinY=[double]::Parse($taskHintMatch.Groups[4].Value,$taskInvariant)
$taskHintMaxX=[double]::Parse($taskHintMatch.Groups[5].Value,$taskInvariant)
$taskHintMaxY=[double]::Parse($taskHintMatch.Groups[6].Value,$taskInvariant)
$taskHintUnit=[double]::Parse($taskHintMatch.Groups[7].Value,$taskInvariant)
$taskHintRotation=[int]$taskHintMatch.Groups[8].Value
$taskWantWidth=if($taskHintRotation -eq 0 -or $taskHintRotation -eq 2){8.0}else{4.0}
$taskWantHeight=if($taskHintRotation -eq 0 -or $taskHintRotation -eq 2){4.0}else{8.0}
$taskHintWidth=$taskHintMaxX-$taskHintMinX
$taskHintHeight=$taskHintMaxY-$taskHintMinY
$taskHintTolerance=[Math]::Max(2.0,$taskHintUnit*0.2)
if([Math]::Abs($taskHintWidth-$taskWantWidth*$taskHintUnit) -gt $taskHintTolerance -or
   [Math]::Abs($taskHintHeight-$taskWantHeight*$taskHintUnit) -gt $taskHintTolerance){
    throw "The selection hint does not follow the declared footprint: $($taskHintMatch.Value)"
}
if([Math]::Abs(($taskHintMinX+$taskHintMaxX)/2-$taskHintCentreX) -gt $taskHintTolerance -or
   [Math]::Abs(($taskHintMinY+$taskHintMaxY)/2-$taskHintCentreY) -gt $taskHintTolerance){
    throw "The selection hint is not centred on the component: $($taskHintMatch.Value)"
}
"PASS text-box selection hint follows footprint width=$([Math]::Round($taskHintWidth,1))px height=$([Math]::Round($taskHintHeight,1))px unit=$taskHintUnit rotation=$taskHintRotation"
Get-Content -LiteralPath $taskLog |
    Select-String 'text-box|Text note|registered|Native logic|Captured frame screenshot|Plugin callback threw|Native failed' |
    Select-Object -Last 25 | ForEach-Object { $_.Line }
if(Test-Path -LiteralPath $taskShot){
    Add-Type -AssemblyName System.Drawing
    $taskImage=[System.Drawing.Bitmap]::FromFile($taskShot)
    # The last report wins: a diagnostic that moves the camera after the first one
    # re-reports the centres, and the captured frame is the one measured here.
    $taskProbeMatches=[regex]::Matches(
        $taskText,
        'PASS text-box default drawing probe centers hidden=([-0-9.]+),([-0-9.]+) visible=([-0-9.]+),([-0-9.]+)'
    )
    $taskProbeMatch=$taskProbeMatches[$taskProbeMatches.Count-1]
    if(-not $taskProbeMatch.Success){throw 'Default drawing probe centers were not parseable'}
    $taskInvariant=[Globalization.CultureInfo]::InvariantCulture
    $taskHiddenX=[Math]::Round([double]::Parse($taskProbeMatch.Groups[1].Value,$taskInvariant))
    $taskHiddenY=[Math]::Round([double]::Parse($taskProbeMatch.Groups[2].Value,$taskInvariant))
    $taskVisibleX=[Math]::Round([double]::Parse($taskProbeMatch.Groups[3].Value,$taskInvariant))
    $taskVisibleY=[Math]::Round([double]::Parse($taskProbeMatch.Groups[4].Value,$taskInvariant))
    function Get-TaskDefaultLabelPixels {
        param([System.Drawing.Bitmap]$Bitmap,[int]$CenterX,[int]$CenterY)
        $count=0
        # The window stays inside the note's own dark box (one board cell is only a
        # handful of pixels when the board is zoomed out): the game's thumbnail and
        # name watermark sit on the component centre, the board's decorative traces
        # are outside this radius.
        $minX=[Math]::Max(0,$CenterX-45);$maxX=[Math]::Min($Bitmap.Width-1,$CenterX+45)
        $minY=[Math]::Max(0,$CenterY-18);$maxY=[Math]::Min($Bitmap.Height-1,$CenterY+18)
        for($y=$minY;$y -le $maxY;$y++){
            for($x=$minX;$x -le $maxX;$x++){
                $pixel=$Bitmap.GetPixel($x,$y)
                # The game's custom-component thumbnail/name uses cyan/teal
                # pixels.  The separately rendered pink electrical pin is
                # deliberately excluded from this measurement.
                if($pixel.G -gt 120 -and $pixel.B -gt 120 -and $pixel.R -lt 190){$count++}
            }
        }
        $count
    }
    $taskHiddenDefaultPixels=Get-TaskDefaultLabelPixels $taskImage $taskHiddenX $taskHiddenY
    $taskVisibleDefaultPixels=Get-TaskDefaultLabelPixels $taskImage $taskVisibleX $taskVisibleY
    # The game's thumbnail is drawn in board units, so "the control still shows its
    # drawing" has to be a zoom-relative count: 150 cyan pixels at the standard
    # 25.6 px/cell, scaled by the square of the zoom.  The unit comes from the last
    # hint line, i.e. from the frame that was captured, not from the first one.
    $taskCaptureUnits=@(Get-Content -LiteralPath $taskLog |
        Select-String 'PASS text-box selection hint instance=' | ForEach-Object {
            if($_.Line -match 'unit=([-0-9.]+)'){[double]$Matches[1]}
        })
    $taskCaptureUnit=if($taskCaptureUnits.Count -gt 0){$taskCaptureUnits[-1]}else{$taskHintUnit}
    $taskVisibleWanted=[Math]::Max(20,[int][Math]::Round(150*[Math]::Pow($taskCaptureUnit/25.6,2)))
    if($taskHiddenDefaultPixels -gt 4 -or $taskVisibleDefaultPixels -lt $taskVisibleWanted){
        throw "Default drawing pixels invalid: hidden=$taskHiddenDefaultPixels visible=$taskVisibleDefaultPixels want>=$taskVisibleWanted"
    }
    "PASS text-box default drawing pixels hidden=$taskHiddenDefaultPixels visible=$taskVisibleDefaultPixels want>=$taskVisibleWanted unit=$([Math]::Round($taskCaptureUnit,2))"
    function Get-TaskMarkerBounds {
        param(
            [System.Drawing.Bitmap]$Bitmap,
            [int]$Red,[int]$Green,[int]$Blue,
            [int]$MinX,[int]$MinY,[int]$MaxX,[int]$MaxY
        )
        $count=0;$foundMinX=[int]::MaxValue;$foundMinY=[int]::MaxValue
        $foundMaxX=[int]::MinValue;$foundMaxY=[int]::MinValue
        for($y=$MinY;$y -le $MaxY;$y++){
            for($x=$MinX;$x -le $MaxX;$x++){
                $pixel=$Bitmap.GetPixel($x,$y)
                if([Math]::Abs([int]$pixel.R-$Red) -le 3 -and
                   [Math]::Abs([int]$pixel.G-$Green) -le 3 -and
                   [Math]::Abs([int]$pixel.B-$Blue) -le 3){
                    $count++
                    if($x -lt $foundMinX){$foundMinX=$x};if($x -gt $foundMaxX){$foundMaxX=$x}
                    if($y -lt $foundMinY){$foundMinY=$y};if($y -gt $foundMaxY){$foundMaxY=$y}
                }
            }
        }
        [pscustomobject]@{Count=$count;MinX=$foundMinX;MinY=$foundMinY;MaxX=$foundMaxX;MaxY=$foundMaxY}
    }
    $taskLeft=Get-TaskMarkerBounds $taskImage 251 1 197 760 232 840 280
    $taskRight=Get-TaskMarkerBounds $taskImage 253 211 3 1760 272 1840 320
    $taskTop=Get-TaskMarkerBounds $taskImage 2 227 251 872 160 920 240
    $taskBottom=Get-TaskMarkerBounds $taskImage 7 249 83 912 810 960 890
    if($taskLeft.Count -lt 700 -or $taskLeft.MinX -lt 799 -or $taskLeft.MinX -gt 801 -or
       $taskLeft.MaxX -lt 830 -or $taskLeft.MaxX -gt 832){throw "Left clip pixels invalid: $($taskLeft|ConvertTo-Json -Compress)"}
    if($taskRight.Count -lt 700 -or $taskRight.MinX -lt 1768 -or $taskRight.MinX -gt 1770 -or
       $taskRight.MaxX -lt 1798 -or $taskRight.MaxX -gt 1800){throw "Right clip pixels invalid: $($taskRight|ConvertTo-Json -Compress)"}
    if($taskTop.Count -lt 700 -or $taskTop.MinY -lt 199 -or $taskTop.MinY -gt 201 -or
       $taskTop.MaxY -lt 230 -or $taskTop.MaxY -gt 232){throw "Top clip pixels invalid: $($taskTop|ConvertTo-Json -Compress)"}
    if($taskBottom.Count -lt 700 -or $taskBottom.MinY -lt 818 -or $taskBottom.MinY -gt 820 -or
       $taskBottom.MaxY -lt 848 -or $taskBottom.MaxY -gt 850){throw "Bottom clip pixels invalid: $($taskBottom|ConvertTo-Json -Compress)"}
    "PASS text-box render clip pixels left=$($taskLeft.MinX)..$($taskLeft.MaxX) right=$($taskRight.MinX)..$($taskRight.MaxX) top=$($taskTop.MinY)..$($taskTop.MaxY) bottom=$($taskBottom.MinY)..$($taskBottom.MaxY)"
    if($taskKeepSelection){
        # Two instances report a footprint window while the arc scenario runs: the
        # suppressed note (selected while it drew its own footprint ring, whose game
        # arc has to be gone) and the unsuppressed control probe (selected when this
        # frame was captured, so the game's arc has to still be there).  Both are
        # measured in this one picture, with the same rule as measure-arc.ps1: the
        # reported box inset by 18 px, because the Mod's own ring sits on the border.
        $taskHintByInstance=@{}
        $taskHintOrder=New-Object System.Collections.ArrayList
        foreach($taskLine in (Get-Content -LiteralPath $taskLog |
                Select-String 'PASS text-box selection hint instance=' | ForEach-Object { $_.Line })){
            $taskHint=[regex]::Match($taskLine,
                'instance=([0-9]+) centre=([-0-9.]+),([-0-9.]+).*box=([-0-9.]+),([-0-9.]+)\.\.([-0-9.]+),([-0-9.]+) unit=([-0-9.]+)')
            if(!$taskHint.Success){continue}
            $taskKey=$taskHint.Groups[1].Value
            if(-not $taskHintByInstance.ContainsKey($taskKey)){[void]$taskHintOrder.Add($taskKey)}
            $taskHintByInstance[$taskKey]=$taskHint
        }
        # The measured instance is the one the hidden-note drag grabbed (its id is on
        # the drag line), and it has to still be the last instance the Mod reported a
        # hint for - that is what proves something was selected when the frame was
        # taken, so a 0 pixel window means "the game's arc is off", not "nothing was
        # selected".
        $taskDragInstance=[regex]::Match($taskText,
            'PASS text-box hidden note drag from [-0-9]+,[-0-9]+ to [-0-9]+,[-0-9]+ selection=[0-9]+ instance=([0-9]+)')
        if(-not $taskDragInstance.Success){throw 'The dragged instance id was not parseable'}
        $taskSuppressedId=$taskDragInstance.Groups[1].Value
        $taskLastHintId=$taskHintOrder[$taskHintOrder.Count-1]
        if(-not $taskHintByInstance.ContainsKey($taskSuppressedId)){
            throw "The dragged instance $taskSuppressedId reported no selection hint"
        }
        if($taskSuppressedId -ne $taskLastHintId){
            throw "The captured frame had instance $taskLastHintId selected instead of the dragged note $taskSuppressedId"
        }
        function Get-TaskArcPixels {
            param([System.Drawing.Bitmap]$Bitmap,[System.Text.RegularExpressions.Match]$Hint)
            $taskUnit=[double]::Parse($Hint.Groups[8].Value,$taskInvariant)
            $taskInset=[int][Math]::Max(18,[Math]::Round($taskUnit*0.65))
            $taskMinX=[int][Math]::Round([double]::Parse($Hint.Groups[4].Value,$taskInvariant))+$taskInset
            $taskMinY=[int][Math]::Round([double]::Parse($Hint.Groups[5].Value,$taskInvariant))+$taskInset
            $taskMaxX=[int][Math]::Round([double]::Parse($Hint.Groups[6].Value,$taskInvariant))-$taskInset
            $taskMaxY=[int][Math]::Round([double]::Parse($Hint.Groups[7].Value,$taskInvariant))-$taskInset
            $count=0
            for($y=$taskMinY;$y -le $taskMaxY;$y++){
                for($x=$taskMinX;$x -le $taskMaxX;$x++){
                    $pixel=$Bitmap.GetPixel($x,$y)
                    if($pixel.R -gt 200 -and $pixel.G -gt 200 -and $pixel.B -gt 200){$count++}
                }
            }
            $count
        }
        $taskNoteArc=Get-TaskArcPixels $taskImage $taskHintByInstance[$taskSuppressedId]
        # TC_TEXTBOX_KEEP_ARCS=1 is the sensitivity control: the Mod leaves the
        # game's selection hint alone for the note, so the very same window has to
        # fill with the arc again.  That is what makes the 0 above a measurement
        # instead of a window that never had any pixels.
        if($taskKeepArcs){
            if($taskNoteArc -lt 100){
                throw "The control run lost the game's selection arc: $taskNoteArc pixel(s)"
            }
        }elseif($taskNoteArc -gt 4){
            throw "The suppressed note still carries the game's selection arc: $taskNoteArc pixel(s)"
        }
        "PASS text-box game arc window=$taskNoteArc pixels instance=$taskSuppressedId selected_at_capture=1 arcs_kept=$(if($taskKeepArcs){1}else{0})"
        # The foundry button's rect, measured in the same picture: 0 pixels when the
        # Mod dropped it, hundreds when the control run kept the game's button.
        if($taskFoundry.Success){
            $taskFoundryMinX=[int][Math]::Round([double]::Parse($taskFoundry.Groups[1].Value,$taskInvariant))
            $taskFoundryMinY=[int][Math]::Round([double]::Parse($taskFoundry.Groups[2].Value,$taskInvariant))
            $taskFoundryMaxX=[int][Math]::Round([double]::Parse($taskFoundry.Groups[3].Value,$taskInvariant))
            $taskFoundryMaxY=[int][Math]::Round([double]::Parse($taskFoundry.Groups[4].Value,$taskInvariant))
            $taskFoundryPixels=0
            for($y=$taskFoundryMinY;$y -le $taskFoundryMaxY;$y++){
                for($x=$taskFoundryMinX;$x -le $taskFoundryMaxX;$x++){
                    $pixel=$taskImage.GetPixel($x,$y)
                    if($pixel.R -gt 90 -and $pixel.G -gt 90 -and $pixel.B -gt 90){$taskFoundryPixels++}
                }
            }
            if($taskFoundryKept){
                if($taskFoundryPixels -lt 100){
                    throw "The control run lost the game's foundry button: $taskFoundryPixels pixel(s)"
                }
            }elseif($taskFoundryPixels -gt 20){
                throw "The game's foundry button is still painted for the Mod component: $taskFoundryPixels pixel(s)"
            }
            "PASS text-box foundry button pixels=$taskFoundryPixels rect=$taskFoundryMinX,$taskFoundryMinY..$taskFoundryMaxX,$taskFoundryMaxY hidden=$(if($taskFoundryHidden){1}else{0})"
        }
    }
    $taskPng=Join-Path $taskOut 'board.png'
    $taskImage.Save($taskPng,[System.Drawing.Imaging.ImageFormat]::Png)
    $taskImage.Dispose()
    "Screenshot: $taskPng"
}else{
    'No screenshot was taken'
}
"Sandbox: $taskSandbox"
