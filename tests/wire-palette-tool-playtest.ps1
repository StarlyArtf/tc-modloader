# End-to-end check of the wire palette's tool-column form: builds the mod, runs
# it in an isolated copy of the game beside a driver that enters a board and
# holds the mouse over the tile, and takes a screenshot from inside the process
# (the sandbox window is parked off the desktop, so nothing outside can see it).
#
# The sandbox needs one extra step no other playtest needs: the live install's
# asset/shader/*.vert are already patched by this very mod, so copying them and
# applying the mod again patches them twice and the game exits at startup.  The
# originals are restored from the loader's blob store before apply, which is the
# documented order (docs/verification.md, "沙箱机制").
param(
  [int]$Seconds = 40,
  [int]$ShotDelay = 16000,
  [string]$Point = '',
  [int]$LeaveAfterMs = 0,
  [string]$Name = 'shot'
)
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskCompiler=if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskGame=Split-Path $taskRepo
$taskSandbox=Join-Path $taskRepo 'build\wire-palette-tool-sandbox'
$taskRoot=Join-Path $taskSandbox 'game'
$taskOut=Join-Path $taskRepo 'build\wire-palette-tool-out'

# 1) The palette itself (this also runs its unit tests).
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $taskRepo 'build-wire-palette.ps1')
if ($LASTEXITCODE) { throw 'Wire palette build failed' }

# 2) The hover driver.
$taskPackage=Join-Path $taskRepo 'build\toolbar-hover-package'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native'),$taskOut | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared (Join-Path $PSScriptRoot 'toolbar-hover-driver.cpp') -o (Join-Path $taskPackage 'native\hover.dll')
if($LASTEXITCODE){throw 'Hover driver build failed'}
'{"format":2,"id":"dev.toolbar-hover","name":"Toolbar hover driver","version":"0.1.0","native":{"api":1,"entry":"native/hover.dll"}}' | Set-Content (Join-Path $taskPackage 'mod.json') -Encoding ascii
if(Test-Path -LiteralPath (Join-Path $taskRepo 'dist\dev.toolbar-hover.mod')){Remove-Item -LiteralPath (Join-Path $taskRepo 'dist\dev.toolbar-hover.mod')}
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRepo 'dist\dev.toolbar-hover.mod')

# 3) Sandbox: same shape as tests/make-ui-sandbox.ps1, with the shader restore
#    inserted between the copy and the apply.
$taskMods=@('local.wire-palette','dev.toolbar-hover')
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
foreach ($taskMod in $taskMods) {
    Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\'+$taskMod+'.mod')) -Destination (Join-Path $taskRoot 'mods') -Force
}
$taskState=Get-Content -LiteralPath (Join-Path $taskGame 'tc-modloader-data\state.json') -Raw | ConvertFrom-Json
foreach ($taskFile in $taskState.files.PSObject.Properties) {
    $taskOriginal=$taskFile.Value.original
    $taskBlob=Join-Path $taskGame ('tc-modloader-data\blobs\'+$taskOriginal)
    if (!(Test-Path -LiteralPath $taskBlob)) { throw "Missing blob for an already-patched file: $taskFile" }
    Copy-Item -LiteralPath $taskBlob -Destination (Join-Path $taskRoot $taskFile.Name) -Force
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskMods
if ($LASTEXITCODE) { throw 'Sandbox mod apply failed' }

# 4) Run it and take the picture while the cursor sits on the tile.
$taskLog=Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskShot=Join-Path $taskOut ($Name+'.bmp')
Remove-Item -LiteralPath $taskLog,$taskShot -ErrorAction SilentlyContinue
$taskPreviousProfile=$env:USERPROFILE;$taskPreviousAppData=$env:APPDATA
$taskPreviousShot=$env:TC_MODLOADER_SHOT;$taskPreviousDelay=$env:TC_MODLOADER_SHOT_DELAY
$taskPreviousPoint=$env:TC_TOOLBAR_POINT;$taskPreviousLeave=$env:TC_TOOLHOVER_LEAVE;$taskProcess=$null
try {
    $env:USERPROFILE=Join-Path $taskSandbox 'home'
    $env:APPDATA=Join-Path $env:USERPROFILE 'AppData\Roaming'
    $env:TC_MODLOADER_SHOT=$taskShot
    $env:TC_MODLOADER_SHOT_DELAY="$ShotDelay"
    if ($Point) { $env:TC_TOOLBAR_POINT=$Point } else { $env:TC_TOOLBAR_POINT=$null }
    if ($LeaveAfterMs -gt 0) { $env:TC_TOOLHOVER_LEAVE="$LeaveAfterMs" } else { $env:TC_TOOLHOVER_LEAVE=$null }
    $taskProcess=Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline=(Get-Date).AddSeconds($Seconds)
    do {
        Start-Sleep -Milliseconds 500
        $taskText=if(Test-Path -LiteralPath $taskLog){Get-Content -LiteralPath $taskLog -Raw}else{''}
        if($taskText -match 'Captured frame screenshot' -or $taskProcess.HasExited){break}
    }while((Get-Date)-lt $taskDeadline)
}finally{
    $env:USERPROFILE=$taskPreviousProfile;$env:APPDATA=$taskPreviousAppData
    $env:TC_MODLOADER_SHOT=$taskPreviousShot;$env:TC_MODLOADER_SHOT_DELAY=$taskPreviousDelay
    $env:TC_TOOLBAR_POINT=$taskPreviousPoint
    $env:TC_TOOLHOVER_LEAVE=$taskPreviousLeave
    if($taskProcess -and !$taskProcess.HasExited){[void]$taskProcess.CloseMainWindow();if(!$taskProcess.WaitForExit(3000)){Stop-Process -Id $taskProcess.Id}}
}
if(!(Test-Path -LiteralPath $taskLog)){throw 'No loader log: the game did not start'}
Get-Content -LiteralPath $taskLog | Select-String 'TOOLHOVER|Tool column text font|Board tool|Wire Palette|Captured frame screenshot|Native failed|Native loaded' | ForEach-Object { $_.Line }
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
