param(
    [string]$GameRoot = 'D:\p',
    [int]$Seconds = 45,
    [int]$ShotDelay = 18000,
    [string]$Name = 'workshop-live',
    [string]$Level = 'foundry',
    # Value handed to TC_MODLOADER_PIN_ORDER_LOG: '1' is the Mod's own summary
    # (one line per panel change), 'dump' also prints the raw cached groups once.
    [string]$PinLog = '1'
)

$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = [IO.Path]::GetFullPath($GameRoot)
$taskBuild = [IO.Path]::GetFullPath((Join-Path $taskRepo 'build'))
$taskRun = Join-Path $taskBuild ('live-root-' + $Name + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$taskHome = Join-Path $taskRun 'home'
$taskOut = Join-Path $taskRun 'out'
$taskShot = Join-Path $taskOut 'workshop-live.bmp'
$taskLog = Join-Path $taskGame 'tc-modloader-data\loader.log'
$taskState = Join-Path $taskGame 'tc-modloader-data\state.json'
$taskCli = Join-Path $taskGame 'tools\tcmod-cli.exe'
$taskDriverId = if ($Level) { 'example.byte-adder' } else { 'dev.enter-board' }
$taskDriver = Join-Path $taskGame ("mods\$taskDriverId.mod")
$taskAutoDir = Join-Path $taskGame 'tc-modloader-data\plugin-data\example.byte-adder'
$taskAutoFile = Join-Path $taskAutoDir 'autotest.txt'

foreach ($required in @('Turing Complete.exe', 'tc_game_engine.dll', 'game_engine.dll',
        'tc-modloader-data\saves.ini', 'tc-modloader-data\state.json', 'tools\tcmod-cli.exe')) {
    if (!(Test-Path -LiteralPath (Join-Path $taskGame $required) -PathType Leaf)) {
        throw "Live-root playtest prerequisite is missing: $required"
    }
}

$taskRunning = Get-CimInstance Win32_Process | Where-Object {
    $_.ExecutablePath -eq (Join-Path $taskGame 'Turing Complete.exe')
}
if ($taskRunning) {
    throw "The live game is already running (PID $($taskRunning.ProcessId -join ',')); close it first"
}

New-Item -ItemType Directory -Force $taskHome, $taskOut | Out-Null
$taskProfileName = [regex]::Match(
    (Get-Content -LiteralPath (Join-Path $taskGame 'tc-modloader-data\saves.ini') -Raw),
    '(?m)^profile=(.+)$').Groups[1].Value.Trim()
if (!$taskProfileName) { throw 'The selected Mod save profile could not be read' }
$taskSourceProfile = Join-Path $env:APPDATA "Turing Complete Mods\profiles\$taskProfileName"
$taskTargetProfiles = Join-Path $taskHome 'AppData\Roaming\Turing Complete Mods\profiles'
if (!(Test-Path -LiteralPath $taskSourceProfile -PathType Container)) {
    throw "The selected Mod save profile is missing: $taskSourceProfile"
}
New-Item -ItemType Directory -Force $taskTargetProfiles | Out-Null
Copy-Item -LiteralPath $taskSourceProfile -Destination $taskTargetProfiles -Recurse

$taskStateBefore = Get-Content -LiteralPath $taskState -Raw | ConvertFrom-Json
$taskEnabledBefore = @($taskStateBefore.enabled)
$taskDriverExisted = Test-Path -LiteralPath $taskDriver
if (!$taskDriverExisted) {
    Copy-Item -LiteralPath (Join-Path $taskRepo "dist\$taskDriverId.mod") -Destination $taskDriver
}
$taskAutoExisted = Test-Path -LiteralPath $taskAutoFile
$taskAutoBefore = if ($taskAutoExisted) { Get-Content -LiteralPath $taskAutoFile -Raw } else { $null }
if ($Level) {
    New-Item -ItemType Directory -Force $taskAutoDir | Out-Null
    Set-Content -LiteralPath $taskAutoFile -Value $Level -Encoding ascii
}
$taskLogStart = (Get-Item -LiteralPath $taskLog).Length

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousShot = $env:TC_MODLOADER_SHOT
$taskPreviousDelay = $env:TC_MODLOADER_SHOT_DELAY
$taskPreviousPunch = $env:TC_MODLOADER_PUNCH_TAPE_LOG
$taskPreviousPin = $env:TC_MODLOADER_PIN_ORDER_LOG
$taskProcess = $null
try {
    & $taskCli $taskGame apply @($taskEnabledBefore + $taskDriverId)
    if ($LASTEXITCODE) { throw "Live apply with $taskDriverId failed" }

    $env:USERPROFILE = $taskHome
    $env:APPDATA = Join-Path $taskHome 'AppData\Roaming'
    $env:TC_MODLOADER_SHOT = $taskShot
    $env:TC_MODLOADER_SHOT_DELAY = "$ShotDelay"
    $env:TC_MODLOADER_PUNCH_TAPE_LOG = '1'
    $env:TC_MODLOADER_PIN_ORDER_LOG = $PinLog
    $taskProcess = Start-Process -FilePath (Join-Path $taskGame 'Turing Complete.exe') `
        -WorkingDirectory $taskGame -WindowStyle Hidden -PassThru
    $taskDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $taskDeadline) {
        Start-Sleep -Milliseconds 500
        if (Test-Path -LiteralPath $taskShot) { break }
        if ($taskProcess.HasExited) { break }
    }
    if (!(Test-Path -LiteralPath $taskShot)) {
        throw "The live-root frame was not captured; exited=$($taskProcess.HasExited)"
    }
} finally {
    $env:USERPROFILE = $taskPreviousProfile
    $env:APPDATA = $taskPreviousAppData
    $env:TC_MODLOADER_SHOT = $taskPreviousShot
    $env:TC_MODLOADER_SHOT_DELAY = $taskPreviousDelay
    $env:TC_MODLOADER_PUNCH_TAPE_LOG = $taskPreviousPunch
    $env:TC_MODLOADER_PIN_ORDER_LOG = $taskPreviousPin
    if ($taskProcess -and !$taskProcess.HasExited) {
        [void]$taskProcess.CloseMainWindow()
        if (!$taskProcess.WaitForExit(4000)) {
            Stop-Process -Id $taskProcess.Id -Force -ErrorAction SilentlyContinue
        }
    }
    & $taskCli $taskGame apply @taskEnabledBefore
    if (!$taskDriverExisted -and (Test-Path -LiteralPath $taskDriver)) {
        Remove-Item -LiteralPath $taskDriver -Force
    }
    if ($Level) {
        if ($taskAutoExisted) {
            Set-Content -LiteralPath $taskAutoFile -Value $taskAutoBefore -NoNewline
        } elseif (Test-Path -LiteralPath $taskAutoFile) {
            Remove-Item -LiteralPath $taskAutoFile -Force
        }
    }
}

Add-Type -AssemblyName System.Drawing
$taskImage = [System.Drawing.Image]::FromFile($taskShot)
$taskPng = Join-Path $taskOut 'workshop-live.png'
$taskImage.Save($taskPng, [System.Drawing.Imaging.ImageFormat]::Png)
$taskSize = "$($taskImage.Width)x$($taskImage.Height)"
$taskImage.Dispose()

$taskStream = [System.IO.File]::Open($taskLog, [System.IO.FileMode]::Open,
    [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
try {
    [void]$taskStream.Seek($taskLogStart, [System.IO.SeekOrigin]::Begin)
    $taskReader = [System.IO.StreamReader]::new($taskStream)
    $taskNewLog = $taskReader.ReadToEnd()
    $taskReader.Dispose()
} finally {
    $taskStream.Dispose()
}
$taskSessionLog = Join-Path $taskOut 'loader-session.log'
Set-Content -LiteralPath $taskSessionLog -Value $taskNewLog -Encoding utf8

$taskNewLog -split "`r?`n" | Where-Object {
    $_ -match 'ENTER-BOARD|byte-adder: autotest|pin order: (inputs|outputs)|punch tape: (anchor|geometry|entry pushed)|Native loaded|TC Mod Loader|Display:'
} | Select-Object -First 200
"Screenshot: $taskPng ($taskSize)"
"Session log: $taskSessionLog"
"Live root: $taskGame"
"Profile copy: $taskProfileName"
"Level: $(if ($Level) { $Level } else { '<home entry>' })"
"Restored Mods: $($taskEnabledBefore -join ',')"
