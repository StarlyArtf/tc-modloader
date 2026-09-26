# Development case for the two-line wide label.  It puts a real board on screen
# (the byte-adder fixture in the campaign's Byte Adder level, driven by the
# example package's autotest) and captures the frame twice:
#
#   * as it is, labels one line tall (whatever the board's word size is);
#   * with tests/word-watchee-wide-probe.cpp forcing every word label to 64
#     bits, which is the path that draws two lines.
#
# Comparing the two pictures is what a person does here; the script's own
# assertions cover the mechanics (the package applied, the probe armed, the
# level ran, frames captured).  It makes no claim about the level's logic.
#
# The probe makes every label 64 bits wide *and* gives it 2^64-1, so the picture
# always shows the hardest case: twenty decimal digits, "1844674407" over
# "3709551615".  A label that shows only the first line's digits twice, or a
# second line with a single digit, is the regression this case exists for.
param(
  [int]$Seconds = 75,
  [int]$ShotDelay = 42000,
  [string]$Name = 'wide',
  [switch]$ReuseSandbox,
  # Value every label is forced to, so the picture has known digits.  Default is
  # 2^64-1 (twenty digits, "1844674407" over "3709551615"); 2^63 is the other
  # number worth eyeballing ("9223372036" over "854775808").
  [string]$Value = '0xFFFFFFFFFFFFFFFF'
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskCompiler = if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskSandbox = Join-Path $taskRepo ('build\word-watchee-layout-' + $Name)
$taskRoot = Join-Path $taskSandbox 'game'
$taskHome = Join-Path $taskSandbox 'home'
$taskOut = Join-Path $taskRepo 'build\word-watchee-layout-out'
$taskFixture = Join-Path $taskRepo 'build\nl_adder8board.data'
if (!(Test-Path -LiteralPath $taskFixture)) { throw "Missing $taskFixture; run build.ps1 first" }

# 1) The wide-label probe, packaged like any other mod.
$taskProbeId = 'test.word-watchee-wide'
$taskProbePackage = Join-Path $taskRepo 'build\word-watchee-wide-package'
if ((Test-Path -LiteralPath $taskSandbox) -and -not $ReuseSandbox) {
    $taskResolved = (Resolve-Path -LiteralPath $taskSandbox).Path
    if (-not $taskResolved.StartsWith((Resolve-Path -LiteralPath (Join-Path $taskRepo 'build')).Path)) {
        throw "Refusing to remove a sandbox outside build/: $taskSandbox"
    }
    Remove-Item -LiteralPath $taskSandbox -Recurse -Force
}
New-Item -ItemType Directory -Force (Join-Path $taskProbePackage 'native'),$taskOut,
    $taskRoot,$taskHome,(Join-Path $taskRoot 'mods') | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -I (Join-Path $taskRepo 'sdk') (Join-Path $PSScriptRoot 'word-watchee-wide-probe.cpp') -o (Join-Path $taskProbePackage 'native\probe.dll')
if($LASTEXITCODE){throw 'Wide-label probe build failed'}
("{`"format`":2,`"id`":`"$taskProbeId`",`"name`":`"Wide label probe`",`"version`":`"0.1.0`",`"native`":{`"api`":1,`"entry`":`"native/probe.dll`"}}") | Set-Content -LiteralPath (Join-Path $taskProbePackage 'mod.json') -Encoding ascii
$taskProbeMod = Join-Path $taskRepo ('dist\' + $taskProbeId + '.mod')
if(Test-Path -LiteralPath $taskProbeMod){Remove-Item -LiteralPath $taskProbeMod}
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskProbePackage -Output $taskProbeMod | Out-Null

# 2) A game copy with the real Byte Adder board, the fix package and the probe.
if (-not $ReuseSandbox) {
    $taskSchema = Join-Path $taskHome "AppData\Roaming\Turing Complete Mods\profiles\default\schematics\byte_adder\Default"
    New-Item -ItemType Directory -Force $taskSchema | Out-Null
    foreach($taskDir in @('asset','campaign','translations')) {
        Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
    }
    foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
        Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
    }
    Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
    Copy-Item -LiteralPath $taskFixture -Destination (Join-Path $taskSchema 'circuit.data')
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\example.byte-adder.mod') -Destination (Join-Path $taskRoot 'mods') -Force
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\local.word-watchee-64.mod') -Destination (Join-Path $taskRoot 'mods') -Force
Copy-Item -LiteralPath $taskProbeMod -Destination (Join-Path $taskRoot 'mods') -Force
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.byte-adder'
New-Item -ItemType Directory -Force $taskData | Out-Null
Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value 'byte_adder' -Encoding ascii
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply example.byte-adder local.word-watchee-64 $taskProbeId
if($LASTEXITCODE){throw 'Layout sandbox apply failed'}

# 3) Run and capture while the level is on screen.
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskShot = Join-Path $taskOut ($Name + '.bmp')
Remove-Item -LiteralPath $taskLog,$taskShot -ErrorAction SilentlyContinue
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousShot = $env:TC_MODLOADER_SHOT
$taskPreviousDelay = $env:TC_MODLOADER_SHOT_DELAY
$taskPreviousWideValue = $env:TC_WATCHEE_WIDE_VALUE
$taskProcess = $null
try {
    $env:USERPROFILE = $taskHome
    $env:APPDATA = Join-Path $taskHome 'AppData\Roaming'
    $env:TC_MODLOADER_SHOT = $taskShot
    $env:TC_MODLOADER_SHOT_DELAY = "$ShotDelay"
    $env:TC_WATCHEE_WIDE_VALUE = $Value
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    $taskExitNote = ''
    do {
        Start-Sleep -Milliseconds 500
        $taskText = if(Test-Path -LiteralPath $taskLog){Get-Content -LiteralPath $taskLog -Raw}else{''}
        if($taskText -match 'Captured frame screenshot'){break}
        # The game may hand the window to a fresh process on its first start in a
        # new profile; that is not a failure, so note it and keep polling for the
        # frame instead of giving up on the picture.
        if($taskProcess.HasExited -and !$taskExitNote){
            $taskExitNote = "the started process ended (exit code $($taskProcess.ExitCode))"
        }
    } while([DateTime]::UtcNow -lt $taskDeadline)
    if($taskExitNote){ "NOTE: $taskExitNote; polling continued" }
} finally {
    $env:USERPROFILE = $taskPreviousProfile
    $env:APPDATA = $taskPreviousAppData
    $env:TC_MODLOADER_SHOT = $taskPreviousShot
    $env:TC_MODLOADER_SHOT_DELAY = $taskPreviousDelay
    $env:TC_WATCHEE_WIDE_VALUE = $taskPreviousWideValue
    if($taskProcess -and !$taskProcess.HasExited) {
        [void]$taskProcess.CloseMainWindow()
        if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
    }
}
if(!(Test-Path -LiteralPath $taskLog)){throw 'No loader log: the game did not start'}
$taskLines = Get-Content -LiteralPath $taskLog
foreach($taskExpected in @(
    'word-watchee-64: value_size ceiling 32 -> 64',
    'wide-label probe: every word label is drawn as 64 bits',
    'byte-adder: autotest finished cycle=')) {
    if(!($taskLines | Where-Object { $_ -match $taskExpected })) {
        $taskLines | Where-Object { $_ -match 'word-watchee|wide-label|byte-adder' } | Select-Object -Last 6
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
