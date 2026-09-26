# Real-machine case for TC_SERVICE_COMPONENT_REGISTRY.
#
# It loads a Mod that registers a component the loader must refuse and the
# byte-adder example (whose component the loader bridges), then reads the
# catalogue through the service: every type, its pins and the refusal reason.
param(
  [int]$Seconds = 90,
  [int]$ShotDelay = 38000,
  [string]$Name = 'registry',
  [string]$Level = 'byte_adder',
  [switch]$ReuseSandbox
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskCompiler = if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskSandbox = Join-Path $taskRepo ('build\component-registry-' + $Name)
$taskRoot = Join-Path $taskSandbox 'game'
$taskHome = Join-Path $taskSandbox 'home'
$taskOut = Join-Path $taskRepo 'build\component-registry-out'
$taskProbeId = 'dev.component-registry-probe'

if ((Test-Path -LiteralPath $taskSandbox) -and -not $ReuseSandbox) {
    $taskResolved = (Resolve-Path -LiteralPath $taskSandbox).Path
    if (-not $taskResolved.StartsWith((Resolve-Path -LiteralPath (Join-Path $taskRepo 'build')).Path)) {
        throw "Refusing to remove a sandbox outside build/: $taskSandbox"
    }
    Remove-Item -LiteralPath $taskSandbox -Recurse -Force
}
New-Item -ItemType Directory -Force $taskRoot,$taskHome,(Join-Path $taskRoot 'mods'),$taskOut | Out-Null
if (-not $ReuseSandbox) {
    foreach($taskDir in @('asset','campaign','translations')) {
        Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
    }
    foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
        Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
    }
    Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
}
# The probe is test-only, so its package is built here; the byte-adder example is
# the Mod whose registration the catalogue has to describe.
$taskProbePackage = Join-Path $taskRepo 'build\component-registry-probe-package'
New-Item -ItemType Directory -Force (Join-Path $taskProbePackage 'native') | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk (Join-Path $PSScriptRoot 'component-registry-probe.cpp') -o (Join-Path $taskProbePackage 'native\probe.dll')
if($LASTEXITCODE){throw 'Component registry probe build failed'}
("{`"format`":2,`"id`":`"$taskProbeId`",`"name`":`"Component registry probe`",`"version`":`"0.1.0`",`"capabilities`":[`"log`",`"component`",`"services`"],`"native`":{`"api`":1,`"entry`":`"native/probe.dll`"}}") | Set-Content -LiteralPath (Join-Path $taskProbePackage 'mod.json') -Encoding ascii
$taskProbeMod = Join-Path $taskRepo ('dist\' + $taskProbeId + '.mod')
if(Test-Path -LiteralPath $taskProbeMod){Remove-Item -LiteralPath $taskProbeMod}
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskProbePackage -Output $taskProbeMod | Out-Null
foreach($taskMod in @('example.byte-adder',$taskProbeId)) {
    Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\'+$taskMod+'.mod')) -Destination (Join-Path $taskRoot 'mods') -Force
}
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.byte-adder'
New-Item -ItemType Directory -Force $taskData | Out-Null
Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value $Level -Encoding ascii
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply example.byte-adder $taskProbeId
if($LASTEXITCODE){throw 'Component registry sandbox apply failed'}

$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskShot = Join-Path $taskOut ($Name + '.bmp')
Remove-Item -LiteralPath $taskLog,$taskShot -ErrorAction SilentlyContinue
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousShot = $env:TC_MODLOADER_SHOT
$taskPreviousDelay = $env:TC_MODLOADER_SHOT_DELAY
$taskProcess = $null
try {
    $env:USERPROFILE = $taskHome
    $env:APPDATA = Join-Path $taskHome 'AppData\Roaming'
    $env:TC_MODLOADER_SHOT = $taskShot
    $env:TC_MODLOADER_SHOT_DELAY = "$ShotDelay"
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    while([DateTime]::UtcNow -lt $taskDeadline) {
        Start-Sleep -Milliseconds 500
        if(Test-Path -LiteralPath $taskShot){break}
        if($taskProcess.HasExited){break}
    }
} finally {
    $env:USERPROFILE = $taskPreviousProfile
    $env:APPDATA = $taskPreviousAppData
    $env:TC_MODLOADER_SHOT = $taskPreviousShot
    $env:TC_MODLOADER_SHOT_DELAY = $taskPreviousDelay
    if($taskProcess -and !$taskProcess.HasExited) {
        [void]$taskProcess.CloseMainWindow()
        if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
    }
}
if(!(Test-Path -LiteralPath $taskLog)){throw 'No loader log: the game did not start'}
$taskLines = Get-Content -LiteralPath $taskLog
$taskRequired = @(
    'byte-adder: autotest loaded level',
    'component registry: probe loaded',
    'component registry: registering nine inputs returned -2 (refused)',
    'component registry: catalogue: 2 type(s)',
    'name="Byte Adder" 3in/2out',
    'in[1] name="A" bits=8',
    'status="the definition is malformed'
)
foreach($taskExpected in $taskRequired) {
    if(!($taskLines | Where-Object { $_ -match [regex]::Escape($taskExpected) })) {
        $taskLines | Where-Object { $_ -match 'component registry' } | Select-Object -Last 8
        throw "Missing evidence: $taskExpected"
    }
}
# The bridged example is the interesting one: it has to be active, with the pin
# names its definition declared.
if(!($taskLines | Where-Object { $_ -match 'name="Byte Adder".*active=1' })) {
    throw 'The bridged byte-adder component is not reported as active'
}
if(!(Test-Path -LiteralPath $taskShot)){throw 'No frame was captured'}
Add-Type -AssemblyName System.Drawing
$taskImage = [System.Drawing.Image]::FromFile($taskShot)
$taskPng = Join-Path $taskOut ($Name + '.png')
$taskImage.Save($taskPng, [System.Drawing.Imaging.ImageFormat]::Png)
$taskImage.Dispose()
$taskLines | Where-Object { $_ -match 'component registry' }
"Screenshot: $taskPng"
"Sandbox: $taskSandbox"
