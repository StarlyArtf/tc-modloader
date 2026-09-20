# Development case for the game's pin-name labels: registers the byte adder
# component with deliberately long pin names, puts the existing board fixture in
# a campaign level's save slot, lets the plugin's own autotest load that level
# and takes a screenshot from inside the process.
#
# Used to look at "pin names overlap" before and after a fix; it makes no claim
# about the level's logic (that is byte-adder-smoke.ps1's job).
param(
  [int]$Seconds = 45,
  [int]$ShotDelay = 26000,
  [string]$Name = 'pins',
  # Level the fixture asks the game to load.  With -Profile the level and the
  # component library come from a real profile, which is the only reliable way
  # to get a component whose pins sit on the top and bottom edges.
  [string]$Level = 'byte_adder',
  # Board installed as the level's save slot (the declarative fixture keeps the
  # byte adder's custom id, so the existing board drives it).
  [string]$Board = '',
  # Extra mods to install beside the fixture (e.g. the cursor driver).
  [string[]]$ExtraMods = @(),
  # Game position the cursor driver pins the mouse to.
  [string]$CursorPoint = '',
  [int]$CursorDelay = 0,
  [string]$CursorPoint2 = '',
  [int]$CursorClick2Delay = 0,
  # When set, a component schematic with long top-row pin names is installed as a
  # saved variant of the level (name = this value), so the component workshop's
  # file browser can open it and show the preview being fixed.
  [string]$ComponentVariant = '',
  # Keep the sandbox (profile included) from the previous run: the game writes
  # levels.txt the first time, and only then can the level be pointed at another
  # schematic variant by editing it.
  [switch]$ReuseSandbox,
  # A profile directory to copy in as the sandbox's saved profile.
  [string]$Profile = ''
)
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskCompiler=if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskGame=Split-Path $taskRepo
# One sandbox per run name: a game that hung on an earlier run can keep a file
# handle in its sandbox, and reusing the directory then fails to clean up.
$taskSandbox=Join-Path $taskRepo ('build\pin-label-sandbox-'+$Name)
$taskRoot=Join-Path $taskSandbox 'game'
$taskHome=Join-Path $taskSandbox 'home'
$taskOut=Join-Path $taskRepo 'build\pin-label-out'
$taskModId='dev.long-pins'

# 1) The fixture mod (the byte adder with long pin names).
$taskPackage=Join-Path $taskRepo 'build\long-pin-package'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native'),$taskOut | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -I (Join-Path $taskRepo 'sdk') (Join-Path $PSScriptRoot 'long-pin-fixture.cpp') -o (Join-Path $taskPackage 'native\byte-adder.dll')
if($LASTEXITCODE){throw 'Long pin fixture build failed'}
"{`"format`":2,`"id`":`"$taskModId`",`"name`":`"Long pin fixture`",`"version`":`"0.1.0`",`"native`":{`"api`":1,`"entry`":`"native/byte-adder.dll`"}}" | Set-Content (Join-Path $taskPackage 'mod.json') -Encoding ascii
$taskMod=Join-Path $taskRepo ('dist\'+$taskModId+'.mod')
if(Test-Path -LiteralPath $taskMod){Remove-Item -LiteralPath $taskMod}
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output $taskMod

if (-not $Board) { $Board = Join-Path $taskRepo 'build\nl_adder8board.data' }
$taskBoardFile=$Board
if (!(Test-Path -LiteralPath $taskBoardFile)) { throw "Board fixture missing: $taskBoardFile (run build.ps1)" }

# 1c) Optional extra mods: the cursor driver makes the game's bottom drawer show
#     the component under the cursor, which is the view being worked on.
if ($ExtraMods -contains 'dev.cursor-pin') {
    $taskCursorPackage=Join-Path $taskRepo 'build\cursor-pin-package'
    New-Item -ItemType Directory -Force (Join-Path $taskCursorPackage 'native') | Out-Null
    & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared (Join-Path $PSScriptRoot 'cursor-pin-driver.cpp') -o (Join-Path $taskCursorPackage 'native\cursor.dll')
    if($LASTEXITCODE){throw 'Cursor driver build failed'}
    '{"format":2,"id":"dev.cursor-pin","name":"Cursor pin driver","version":"0.1.0","native":{"api":1,"entry":"native/cursor.dll"}}' | Set-Content (Join-Path $taskCursorPackage 'mod.json') -Encoding ascii
    $taskCursorMod=Join-Path $taskRepo 'dist\dev.cursor-pin.mod'
    if(Test-Path -LiteralPath $taskCursorMod){Remove-Item -LiteralPath $taskCursorMod}
    & (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskCursorPackage -Output $taskCursorMod
}

# 2) Sandbox with the level's save slot holding the board fixture.
if ((Test-Path -LiteralPath $taskSandbox) -and -not $ReuseSandbox) {
    $resolved=(Resolve-Path -LiteralPath $taskSandbox).Path
    if (-not $resolved.StartsWith((Resolve-Path -LiteralPath (Join-Path $taskRepo 'build')).Path)) {
        throw "Refusing to remove a sandbox outside build/: $taskSandbox"
    }
    Remove-Item -LiteralPath $taskSandbox -Recurse -Force
}
$taskSchema=Join-Path $taskHome "AppData\Roaming\Turing Complete Mods\profiles\default\schematics\byte_adder\Default"
$taskData=Join-Path $taskRoot "tc-modloader-data\plugin-data\$taskModId"
New-Item -ItemType Directory -Force $taskRoot,$taskHome,(Join-Path $taskRoot 'mods'),$taskSchema,$taskData,$taskOut | Out-Null
if (-not $ReuseSandbox) {
foreach ($taskDir in @('asset','campaign','translations')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot -Force
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
}
else {
    Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
}
# With -Profile the point is to look at a real player's content, not at the
# fixture (its component id clashes with ids already used in that profile, which
# makes the plugin fail to load), so the fixture is left out then.
$taskApplyIds=@()
if (-not $Profile) {
    Copy-Item -LiteralPath $taskMod -Destination (Join-Path $taskRoot 'mods') -Force
    $taskApplyIds+=$taskModId
}
foreach ($taskExtra in $ExtraMods) {
    $taskExtraFile=Join-Path $taskRepo ('dist\'+$taskExtra+'.mod')
    if (!(Test-Path -LiteralPath $taskExtraFile)) { throw "Extra mod missing: $taskExtraFile" }
    Copy-Item -LiteralPath $taskExtraFile -Destination (Join-Path $taskRoot 'mods') -Force
    $taskApplyIds+=$taskExtra
}
if ($Profile) {
    if (!(Test-Path -LiteralPath $Profile)) { throw "Profile not found: $Profile" }
    $taskProfileTarget=Join-Path $taskHome 'AppData\Roaming\Turing Complete Mods\profiles\default'
    Remove-Item -LiteralPath $taskProfileTarget -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath $Profile -Destination $taskProfileTarget -Recurse -Force
}
else {
    Copy-Item -LiteralPath $taskBoardFile -Destination (Join-Path $taskSchema 'circuit.data') -Force
}
if ($ComponentVariant) {
    $taskGeneratorExe=Join-Path $taskRepo 'build\long-pin-component.exe'
    $taskVariantFile=Join-Path $taskRepo 'build\long-pin-component.data'
    & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -w (Join-Path $PSScriptRoot 'long-pin-component.cpp') -o $taskGeneratorExe
    if($LASTEXITCODE){throw 'Long pin component generator build failed'}
    & $taskGeneratorExe $taskVariantFile
    if($LASTEXITCODE){throw 'Long pin component generation failed'}
    $taskVariantDir=Join-Path $taskHome ("AppData\Roaming\Turing Complete Mods\profiles\default\schematics\$Level\$ComponentVariant")
    New-Item -ItemType Directory -Force $taskVariantDir | Out-Null
    Copy-Item -LiteralPath $taskVariantFile -Destination (Join-Path $taskVariantDir 'circuit.data') -Force
    # Also make it the level's default schematic: a fresh sandbox profile has no
    # levels.txt yet, so pointing the level at a new variant by editing that file
    # does nothing - and the workshop previews the schematic the level has open.
    Copy-Item -LiteralPath $taskVariantFile -Destination (Join-Path $taskSchema 'circuit.data') -Force
    # Point the level at that variant, so both the board and the workshop open it
    # (the workshop's preview is what shows the pin labels).
    $taskLevelsFile=Join-Path $taskHome 'AppData\Roaming\Turing Complete Mods\profiles\default\levels.txt'
    if (Test-Path -LiteralPath $taskLevelsFile) {
        $taskLevels=Get-Content -LiteralPath $taskLevelsFile -Raw
        $taskPattern='("'+[regex]::Escape($Level)+'",(?:true|false),")[^"]*(")'
        $taskLevels=[regex]::Replace($taskLevels,$taskPattern,('${1}'+$ComponentVariant+'${2}'))
        Set-Content -LiteralPath $taskLevelsFile -Value $taskLevels -NoNewline -Encoding utf8
    }
}
Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value $Level -Encoding ascii
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskApplyIds
if ($LASTEXITCODE) { throw 'Sandbox mod apply failed' }

# 3) Run and capture while the level is on screen.
$taskLog=Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskShot=Join-Path $taskOut ($Name+'.bmp')
Remove-Item -LiteralPath $taskLog,$taskShot -ErrorAction SilentlyContinue
$taskPrevProfile=$env:USERPROFILE;$taskPrevAppData=$env:APPDATA
$taskPrevShot=$env:TC_MODLOADER_SHOT;$taskPrevDelay=$env:TC_MODLOADER_SHOT_DELAY
$taskPrevTrace=$env:TC_MODLOADER_TRACE_TEXT
$taskPrevCursorPoint=$env:TC_CURSOR_POINT;$taskPrevCursorDelay=$env:TC_CURSOR_DELAY
$taskPrevCursorPoint2=$env:TC_CURSOR_POINT2;$taskPrevClick2=$env:TC_CURSOR_CLICK2_DELAY
$taskProcess=$null
try {
    $env:USERPROFILE=$taskHome
    $env:APPDATA=Join-Path $taskHome 'AppData\Roaming'
    $env:TC_MODLOADER_SHOT=$taskShot
    $env:TC_MODLOADER_SHOT_DELAY="$ShotDelay"
    $env:TC_MODLOADER_TRACE_TEXT=$env:TC_MODLOADER_TRACE_TEXT
    if ($CursorPoint) { $env:TC_CURSOR_POINT=$CursorPoint } else { $env:TC_CURSOR_POINT=$null }
    if ($CursorDelay -gt 0) { $env:TC_CURSOR_DELAY="$CursorDelay" } else { $env:TC_CURSOR_DELAY=$null }
    if ($CursorPoint2) { $env:TC_CURSOR_POINT2=$CursorPoint2 } else { $env:TC_CURSOR_POINT2=$null }
    if ($CursorClick2Delay -gt 0) { $env:TC_CURSOR_CLICK2_DELAY="$CursorClick2Delay" } else { $env:TC_CURSOR_CLICK2_DELAY=$null }
    $taskProcess=Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline=(Get-Date).AddSeconds($Seconds)
    do {
        Start-Sleep -Milliseconds 500
        $taskText=if(Test-Path -LiteralPath $taskLog){Get-Content -LiteralPath $taskLog -Raw}else{''}
        if($taskText -match 'Captured frame screenshot' -or $taskProcess.HasExited){break}
    }while((Get-Date)-lt $taskDeadline)
}finally{
    $env:USERPROFILE=$taskPrevProfile;$env:APPDATA=$taskPrevAppData
    $env:TC_MODLOADER_SHOT=$taskPrevShot;$env:TC_MODLOADER_SHOT_DELAY=$taskPrevDelay
    $env:TC_MODLOADER_TRACE_TEXT=$taskPrevTrace
    $env:TC_CURSOR_POINT=$taskPrevCursorPoint
    $env:TC_CURSOR_DELAY=$taskPrevCursorDelay
    $env:TC_CURSOR_POINT2=$taskPrevCursorPoint2
    $env:TC_CURSOR_CLICK2_DELAY=$taskPrevClick2
    if($taskProcess -and !$taskProcess.HasExited){[void]$taskProcess.CloseMainWindow();if(!$taskProcess.WaitForExit(3000)){Stop-Process -Id $taskProcess.Id}}
}
if(!(Test-Path -LiteralPath $taskLog)){throw 'No loader log: the game did not start'}
Get-Content -LiteralPath $taskLog | Select-String 'autotest|Captured frame screenshot|Native failed|CURSOR|Markup |Long pin' | Select-Object -First 20 | ForEach-Object { $_.Line }
if(Test-Path -LiteralPath $taskShot){
    Add-Type -AssemblyName System.Drawing
    $taskImage=[System.Drawing.Image]::FromFile($taskShot)
    $taskPng=Join-Path $taskOut ($Name+'.png')
    $taskImage.Save($taskPng,[System.Drawing.Imaging.ImageFormat]::Png)
    $taskImage.Dispose()
    "Screenshot: $taskPng"
}else{
    'No screenshot was taken'
}
