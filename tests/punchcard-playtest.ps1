# Development case for "wide values use the game's own punch tape".
# It enters the 32-bit Symphony IO level, applies tests/punchcard-probe.cpp (which
# raises the UI's 8-bit ceiling to 64 in memory) and captures the frame, so a
# person can see whether the bit toggles appear next to the value.
param(
  [int]$Seconds = 80,
  [int]$ShotDelay = 42000,
  [string]$Name = 'wide',
  [string]$Level = 'symphony_2_io',
  [switch]$NoPatch,
  # Exercise the player-facing Mod instead of the old byte-ceiling probe.  The
  # Mod keeps the native <=8-bit path and adds its own grouped, responsive tape
  # only to wide input rows.
  [switch]$PunchTape,
  [switch]$ReuseSandbox
  ,
  # Exercise the punch tape's mask tool end to end: the Mod applies one mask to
  # the first wide input itself (the same code path the popup's buttons use) and
  # reports the value it read back, so a run can assert it without a human click.
  # Format: "<op>:<expression>", e.g. "or:0x100" or "toggle:0xFFFFFFFF^(1<<23)".
  [string]$Mask = ''
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskCompiler = if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskSandbox = Join-Path $taskRepo ('build\punchcard-' + $Name)
$taskRoot = Join-Path $taskSandbox 'game'
$taskHome = Join-Path $taskSandbox 'home'
$taskOut = Join-Path $taskRepo 'build\punchcard-out'
$taskProbeId = 'test.punchcard-probe'

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
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\local.word-watchee-64.mod') -Destination (Join-Path $taskRoot 'mods') -Force
if ($PunchTape) {
    Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\local.punch-tape.mod') -Destination (Join-Path $taskRoot 'mods') -Force
}

# The probe package (unless this run is the "before" picture).
$taskApply = @('example.byte-adder','local.word-watchee-64')
if ($PunchTape) {
    $taskApply += 'local.punch-tape'
}
elseif (-not $NoPatch) {
    $taskProbePackage = Join-Path $taskRepo 'build\punchcard-probe-package'
    New-Item -ItemType Directory -Force (Join-Path $taskProbePackage 'native') | Out-Null
    & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -I (Join-Path $taskRepo 'sdk') (Join-Path $PSScriptRoot 'punchcard-probe.cpp') -o (Join-Path $taskProbePackage 'native\probe.dll')
    if($LASTEXITCODE){throw 'Punchcard probe build failed'}
    ("{`"format`":2,`"id`":`"$taskProbeId`",`"name`":`"Punchcard probe`",`"version`":`"0.1.0`",`"native`":{`"api`":1,`"entry`":`"native/probe.dll`"}}") | Set-Content -LiteralPath (Join-Path $taskProbePackage 'mod.json') -Encoding ascii
    $taskProbeMod = Join-Path $taskRepo ('dist\' + $taskProbeId + '.mod')
    if(Test-Path -LiteralPath $taskProbeMod){Remove-Item -LiteralPath $taskProbeMod}
    & (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskProbePackage -Output $taskProbeMod | Out-Null
    Copy-Item -LiteralPath $taskProbeMod -Destination (Join-Path $taskRoot 'mods') -Force
    $taskApply += $taskProbeId
}
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.byte-adder'
New-Item -ItemType Directory -Force $taskData | Out-Null
Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value $Level -Encoding ascii
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskApply
if($LASTEXITCODE){throw 'Punchcard sandbox apply failed'}

$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskShot = Join-Path $taskOut ($Name + '.bmp')
Remove-Item -LiteralPath $taskLog,$taskShot -ErrorAction SilentlyContinue
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousShot = $env:TC_MODLOADER_SHOT
$taskPreviousDelay = $env:TC_MODLOADER_SHOT_DELAY
$taskPreviousMask = $env:TC_MODLOADER_PUNCH_TAPE_MASK
$taskProcess = $null
try {
    $env:USERPROFILE = $taskHome
    $env:APPDATA = Join-Path $taskHome 'AppData\Roaming'
    $env:TC_MODLOADER_SHOT = $taskShot
    $env:TC_MODLOADER_SHOT_DELAY = "$ShotDelay"
    if ($Mask) { $env:TC_MODLOADER_PUNCH_TAPE_MASK = $Mask }
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
    $env:TC_MODLOADER_PUNCH_TAPE_MASK = $taskPreviousMask
    if($taskProcess -and !$taskProcess.HasExited) {
        [void]$taskProcess.CloseMainWindow()
        if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
    }
}
if(!(Test-Path -LiteralPath $taskLog)){throw 'No loader log: the game did not start'}
$taskLines = Get-Content -LiteralPath $taskLog
$taskRequired = @('byte-adder: autotest loaded level')
if (-not $NoPatch) { $taskRequired += 'punchcard probe: wide values now draw the game''s punch tape' }
if ($PunchTape) {
    $taskRequired = @('byte-adder: autotest loaded level',
                      'punch tape: workshop input hook installed',
                      'punch tape: geometry workshop_input')
}
if ($Mask) {
    $taskRequired += 'punch tape: mask selftest'
    # The layer ran: the following frame's line reports punch data, what the
    # panel keeps and what the circuit is handed.  A level whose own test drives
    # its inputs can legitimately re-apply them, so the comparison stays
    # informational while "layer=1" is what is asserted.
    $taskRequired += 'layer=1'
}
foreach($taskExpected in $taskRequired) {
    if(!($taskLines | Where-Object { $_ -match [regex]::Escape($taskExpected) })) {
        $taskLines | Select-Object -Last 8
        throw "Missing evidence: $taskExpected"
    }
}
if(!(Test-Path -LiteralPath $taskShot)){throw 'No frame was captured'}
Add-Type -AssemblyName System.Drawing
$taskImage = [System.Drawing.Image]::FromFile($taskShot)
$taskPng = Join-Path $taskOut ($Name + '.png')
$taskImage.Save($taskPng, [System.Drawing.Imaging.ImageFormat]::Png)
$taskImage.Dispose()
"Screenshot: $taskPng"
"Sandbox: $taskSandbox"
