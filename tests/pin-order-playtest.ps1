# Development case for "the player can reorder the IO panel's pins".
#
# It enters an IO level, loads the player-facing Mod (frames + grips + drag) and
# captures the frame, so the handles can be judged in a picture.  With -Probe it
# loads tests/pin-order-probe.cpp instead, which dumps the panel's cached IO
# records - the layout the reordering itself depends on.
param(
  [int]$Seconds = 90,
  [int]$ShotDelay = 40000,
  [string]$Name = 'panel',
  [string]$Level = 'symphony_8_io_devices',
  [switch]$Probe,
  # Play a real drag (posted mouse messages) at the Mod's own handles and check
  # the panel's order afterwards: tests/pin-order-driver.cpp drives it.
  [switch]$Drag,
  # Ask the loader to dump the raw structure of the panel's cached IO records
  # (TC_MODLOADER_PIN_ORDER_LOG=dump): the layout src/pin_order.hpp is read from.
  [switch]$Dump,
  # Extra Mods to load alongside (e.g. local.punch-tape, to see the two next to
  # each other in the same panel).
  [string[]]$ExtraMods = @(),
  [switch]$ReuseSandbox
)
# The panel lists the level's own IO devices, so a drag case needs a level with
# more than one entry per group: the byte adder's level has three inputs and two
# outputs, while the default level (used for the screenshot) has one of each.
if ($Drag -and $Level -eq 'symphony_8_io_devices') { $Level = 'byte_adder' }
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskCompiler = if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskSandbox = Join-Path $taskRepo ('build\pin-order-' + $Name)
$taskRoot = Join-Path $taskSandbox 'game'
$taskHome = Join-Path $taskSandbox 'home'
$taskOut = Join-Path $taskRepo 'build\pin-order-out'
$taskProbeId = 'dev.pin-order-probe'

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
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\example.byte-adder.mod') -Destination (Join-Path $taskRoot 'mods') -Force

$taskApply = @('example.byte-adder')
$ExtraMods = @($ExtraMods | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
foreach ($taskExtra in $ExtraMods) {
    Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\' + $taskExtra + '.mod')) -Destination (Join-Path $taskRoot 'mods') -Force
    $taskApply += $taskExtra
}
if ($Probe) {
    $taskProbePackage = Join-Path $taskRepo 'build\pin-order-probe-package'
    New-Item -ItemType Directory -Force (Join-Path $taskProbePackage 'native') | Out-Null
    & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -I (Join-Path $taskRepo 'sdk') (Join-Path $PSScriptRoot 'pin-order-probe.cpp') -o (Join-Path $taskProbePackage 'native\probe.dll')
    if($LASTEXITCODE){throw 'Pin order probe build failed'}
    ("{`"format`":2,`"id`":`"$taskProbeId`",`"name`":`"Pin order probe`",`"version`":`"0.1.0`",`"capabilities`":[`"log`",`"hook`",`"symbol`"],`"native`":{`"api`":1,`"entry`":`"native/probe.dll`"}}") | Set-Content -LiteralPath (Join-Path $taskProbePackage 'mod.json') -Encoding ascii
    $taskProbeMod = Join-Path $taskRepo ('dist\' + $taskProbeId + '.mod')
    if(Test-Path -LiteralPath $taskProbeMod){Remove-Item -LiteralPath $taskProbeMod}
    & (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskProbePackage -Output $taskProbeMod | Out-Null
    Copy-Item -LiteralPath $taskProbeMod -Destination (Join-Path $taskRoot 'mods') -Force
    $taskApply += $taskProbeId
} else {
    Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\local.pin-order.mod') -Destination (Join-Path $taskRoot 'mods') -Force
    $taskApply += 'local.pin-order'
    if ($Drag) {
        Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\dev.pin-order-driver.mod') -Destination (Join-Path $taskRoot 'mods') -Force
        $taskApply += 'dev.pin-order-driver'
    }
}
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.byte-adder'
New-Item -ItemType Directory -Force $taskData | Out-Null
Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value $Level -Encoding ascii
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskApply
if($LASTEXITCODE){throw 'Pin order sandbox apply failed'}

$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskShot = Join-Path $taskOut ($Name + '.bmp')
Remove-Item -LiteralPath $taskLog,$taskShot -ErrorAction SilentlyContinue
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousShot = $env:TC_MODLOADER_SHOT
$taskPreviousDelay = $env:TC_MODLOADER_SHOT_DELAY
$taskPreviousProbe = $env:TC_MODLOADER_PIN_ORDER_LOG
$taskProcess = $null
try {
    $env:USERPROFILE = $taskHome
    $env:APPDATA = Join-Path $taskHome 'AppData\Roaming'
    $env:TC_MODLOADER_SHOT = $taskShot
    $env:TC_MODLOADER_SHOT_DELAY = "$ShotDelay"
    if ($env:TC_PIN_ORDER_PROBE_LOG) { $env:TC_MODLOADER_PIN_ORDER_LOG = $env:TC_PIN_ORDER_PROBE_LOG }
    elseif ($Dump) { $env:TC_MODLOADER_PIN_ORDER_LOG = 'dump' }
    elseif ($Drag) { $env:TC_MODLOADER_PIN_ORDER_LOG = '1' }
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
    $env:TC_MODLOADER_PIN_ORDER_LOG = $taskPreviousProbe
    if($taskProcess -and !$taskProcess.HasExited) {
        [void]$taskProcess.CloseMainWindow()
        if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
    }
}
if(!(Test-Path -LiteralPath $taskLog)){throw 'No loader log: the game did not start'}
$taskLines = Get-Content -LiteralPath $taskLog
$taskRequired = @('byte-adder: autotest loaded level')
if ($Probe) {
    $taskRequired += 'pin order probe: anchor hook installed'
    $taskRequired += 'pin order service: group=0 count='
} else {
    $taskRequired += 'pin order: entry frame hook installed'
}
if ($Dump) { $taskRequired += 'pin order dump: group 0 count=' }
if ($Drag) {
    # The button the driver presses: one slot down, from the first entry.
    $taskRequired += 'pin order: moved inputs #0 to #1 (status 0)'
    $taskRequired += 'pin order driver: clicked the down button of inputs #0'
    $taskRequired += 'pin order driver: order inputs before=['
}
foreach($taskExpected in $taskRequired) {
    if(!($taskLines | Where-Object { $_ -match [regex]::Escape($taskExpected) })) {
        $taskLines | Select-Object -Last 8
        throw "Missing evidence: $taskExpected"
    }
}
if(!(Test-Path -LiteralPath $taskShot)){throw 'No frame was captured'}
if ($Drag) {
    # The button press has to have moved the panel's order, not just been
    # reported: the driver reads the order back from the service after it.
    $taskOrder = $taskLines | Where-Object { $_ -match 'pin order driver: order inputs before=\[' } |
        Select-Object -Last 1
    $taskMatch = [regex]::Match($taskOrder, 'before=\[([^\]]*)\] after=\[([^\]]*)\]')
    if(-not $taskMatch.Success){ throw 'The driver did not report the order' }
    $taskBefore = $taskMatch.Groups[1].Value
    $taskAfter = $taskMatch.Groups[2].Value
    $taskKeys = $taskBefore -split ','
    # One step down: the first two entries trade places.
    $taskWanted = @($taskKeys[1], $taskKeys[0]) + ($taskKeys | Select-Object -Skip 2)
    $taskWanted = $taskWanted -join ','
    if($taskBefore -eq $taskAfter){ throw "The button did not change the order ($taskBefore)" }
    if($taskAfter -ne $taskWanted){
        throw "The panel shows '$taskAfter' after one step down, expected '$taskWanted'"
    }
    "Order: $taskBefore -> $taskAfter"
}
Add-Type -AssemblyName System.Drawing
$taskImage = [System.Drawing.Image]::FromFile($taskShot)
$taskPng = Join-Path $taskOut ($Name + '.png')
$taskImage.Save($taskPng, [System.Drawing.Imaging.ImageFormat]::Png)
$taskImage.Dispose()
"Screenshot: $taskPng"
"Sandbox: $taskSandbox"
