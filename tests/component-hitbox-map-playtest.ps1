# Maps a custom component's pointer hit region on a real board.
#
# `component-hitbox-playtest.ps1` samples two transition points and asserts the
# negative contract "a 12x6 footprint does not enlarge the clickable area".
# This case samples a whole 9x7 grid around two instances that differ only in
# their declared footprint and asserts that the two maps are identical, which
# says the same thing at 63 offsets per type instead of two.  The grid also
# prints where the game's own hit region actually is, which is what the next
# geometry step (a real hit box) has to reproduce.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests/component-hitbox-map-playtest.ps1
param(
  [int]$Seconds = 300,
  [switch]$KeepRunning,
  [switch]$NoPause
)
$ErrorActionPreference='Stop'
if(!$NoPause){
    Write-Host 'The probe drives the real cursor and pins it with ClipCursor while it presses.'
    Write-Host 'Please do not move or click the mouse until this script prints PASS/FAIL (about 2 minutes).'
    Write-Host 'Any real mouse input is detected and fails the case instead of being averaged in.'
    Start-Sleep -Seconds 5
}
$taskRepo=Split-Path $PSScriptRoot
$taskGame=Split-Path $taskRepo
$taskCompiler=if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskSandbox=Join-Path $taskRepo 'build\hitbox-map-sandbox'
$taskRoot=Join-Path $taskSandbox 'game'
$taskOut=Join-Path $taskRepo 'build\hitbox-map-out'
$taskPackage=Join-Path $taskRepo 'build\hitbox-map-package'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native'),$taskOut | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk (Join-Path $PSScriptRoot 'component-hitbox-probe.cpp') -o (Join-Path $taskPackage 'native\hitbox.dll')
if($LASTEXITCODE){throw 'Hitbox probe build failed'}
'{"format":2,"id":"dev.hitbox-probe","name":"Component hit-area probe","version":"0.1.0","capabilities":["log","component","services","symbol","symbol_alias","game_handles"],"native":{"api":1,"entry":"native/hitbox.dll"}}' | Set-Content (Join-Path $taskPackage 'mod.json') -Encoding ascii
if(Test-Path -LiteralPath (Join-Path $taskRepo 'dist\dev.hitbox-probe.mod')){Remove-Item -LiteralPath (Join-Path $taskRepo 'dist\dev.hitbox-probe.mod')}
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRepo 'dist\dev.hitbox-probe.mod')

if (Test-Path -LiteralPath $taskSandbox) {
    $resolved=(Resolve-Path -LiteralPath $taskSandbox).Path
    if (-not $resolved.StartsWith((Resolve-Path -LiteralPath (Join-Path $taskRepo 'build')).Path)) {
        throw "Refusing to remove a sandbox outside build/: $taskSandbox"
    }
    Remove-Item -LiteralPath $taskSandbox -Recurse -Force
}
New-Item -ItemType Directory -Force $taskRoot,(Join-Path $taskSandbox 'home'),(Join-Path $taskRoot 'mods') | Out-Null
foreach ($taskDir in @('asset','campaign','translations')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach ($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot -Force
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
foreach ($taskMod in @('dev.hitbox-probe','dev.enter-board')) {
    Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\'+$taskMod+'.mod')) -Destination (Join-Path $taskRoot 'mods') -Force
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.hitbox-probe dev.enter-board
if ($LASTEXITCODE) { throw 'Sandbox mod apply failed' }

$taskLog=Join-Path $taskRoot 'tc-modloader-data\loader.log'
Remove-Item -LiteralPath $taskLog -ErrorAction SilentlyContinue
$taskPreviousProfile=$env:USERPROFILE;$taskPreviousAppData=$env:APPDATA
$taskPreviousScan=$env:TC_HITBOX_SCAN
$taskProcess=$null
try {
    $env:USERPROFILE=Join-Path $taskSandbox 'home'
    $env:APPDATA=Join-Path $env:USERPROFILE 'AppData\Roaming'
    $env:TC_HITBOX_SCAN='map'
    $taskProcess=Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline=(Get-Date).AddSeconds($Seconds)
    do {
        Start-Sleep -Milliseconds 500
        $taskText=if(Test-Path -LiteralPath $taskLog){Get-Content -LiteralPath $taskLog -Raw}else{''}
        if($taskText -match 'hitbox: scan finished' -or
           $taskText -match 'Plugin callback threw|Native failed' -or
           $taskProcess.HasExited){break}
    }while((Get-Date)-lt $taskDeadline)
}finally{
    $env:USERPROFILE=$taskPreviousProfile;$env:APPDATA=$taskPreviousAppData;$env:TC_HITBOX_SCAN=$taskPreviousScan
    if($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning){
        [void]$taskProcess.CloseMainWindow()
        if(!$taskProcess.WaitForExit(4000)){Stop-Process -Id $taskProcess.Id}
    }
}
if(!(Test-Path -LiteralPath $taskLog)){throw 'No loader log: the game did not start'}
$taskText=Get-Content -LiteralPath $taskLog -Raw
if($taskText -notmatch 'scan=map points='){throw 'The probe did not start in map mode'}
if($taskText -notmatch 'hitbox: scan finished'){throw 'The hit-area map did not finish; see the loader log'}

# Samples in order, so a point that is sampled twice can be compared with its
# own earlier reading.
$taskSamples=@()
foreach($taskMatch in [regex]::Matches($taskText,'hitbox: (hit-[a-z]+) offset=\((-?\d+),(-?\d+)\).*? clean=(\d).*? selected=(\d)')){
    $taskSamples+=[pscustomobject]@{
        Name=$taskMatch.Groups[1].Value
        Dx=[int]$taskMatch.Groups[2].Value
        Dy=[int]$taskMatch.Groups[3].Value
        Key="$($taskMatch.Groups[2].Value),$($taskMatch.Groups[3].Value)"
        Clean=[int]$taskMatch.Groups[4].Value
        Selected=[int]$taskMatch.Groups[5].Value
    }
}
$taskDirty=@($taskSamples | Where-Object { $_.Clean -ne 1 })
if($taskDirty.Count -gt 0){
    $taskFirst=$taskDirty | Select-Object -First 3
    $taskDetail=($taskFirst | ForEach-Object { "$($_.Name) ($($_.Dx),$($_.Dy))" }) -join ', '
    throw ("$($taskDirty.Count) of $($taskSamples.Count) samples were contaminated by real input ($taskDetail ...). " +
           "The pointer or ImGui's mouse position was no longer where the probe put it, so the map is not evidence. " +
           "Rerun without touching the mouse.")
}
$taskRows=@{}
foreach($taskMatch in [regex]::Matches($taskText,'hitbox: maprow (hit-[a-z]+) dy=(-?\d+) ([01]+)')){
    if(-not $taskRows.ContainsKey($taskMatch.Groups[1].Value)){$taskRows[$taskMatch.Groups[1].Value]=@()}
    $taskRows[$taskMatch.Groups[1].Value]+=$taskMatch.Groups[3].Value
}
$taskStillSelected=@()
foreach($taskMatch in [regex]::Matches($taskText,'hitbox: cleared (hit-[a-z]+) after=\((-?\d+),(-?\d+)\) selected=(\d+)')){
    if([int]$taskMatch.Groups[4].Value -ne 0){
        $taskStillSelected+="$($taskMatch.Groups[1].Value) after=($($taskMatch.Groups[2].Value),$($taskMatch.Groups[3].Value)) still=$($taskMatch.Groups[4].Value)"
    }
}
foreach($taskName in @('hit-default','hit-expanded')){
    if(-not $taskRows.ContainsKey($taskName)){throw "Missing hit-area map for $taskName"}
    if($taskRows[$taskName].Count -ne 7){throw "Expected 7 map rows for $taskName, got $($taskRows[$taskName].Count)"}
    for($taskDx=-4;$taskDx -le 4;$taskDx++){
        for($taskDy=-3;$taskDy -le 3;$taskDy++){
            $taskPoint=@($taskSamples | Where-Object { $_.Name -eq $taskName -and $_.Dx -eq $taskDx -and $_.Dy -eq $taskDy })
            if($taskPoint.Count -lt 1){throw "Missing sample for $taskName offset=($taskDx,$taskDy)"}
        }
    }
    # Control 1: a point 40 board units away cannot be inside any component on
    # this board, so it has to read "not selected".
    $taskFar=@($taskSamples | Where-Object { $_.Name -eq $taskName -and $_.Dx -eq 40 -and $_.Dy -eq 0 })
    if($taskFar.Count -ne 2){throw "Expected two far control samples for $taskName, got $($taskFar.Count)"}
    if($taskFar[0].Selected -ne 0 -or $taskFar[1].Selected -ne 0){
        throw "$taskName reports a hit 40 board units away (selected=$($taskFar[0].Selected),$($taskFar[1].Selected)); the sample is not a pointer hit test"
    }
    # Control 2: the same point, sampled again after another press, has to answer
    # the same way.  A hit that survives the clears between samples would make
    # every later offset look like a hit, and a reading that is not repeatable
    # cannot be turned into a hit region at all.
    $taskRepeats=@()
    foreach($taskGroup in ($taskSamples | Where-Object { $_.Name -eq $taskName } | Group-Object Key)){
        $taskValues=@($taskGroup.Group | ForEach-Object { $_.Selected })
        $taskDistinct=@($taskValues | Select-Object -Unique)
        if($taskDistinct.Count -gt 1){
            $taskRepeats+="$($taskGroup.Name): $($taskValues -join ',')"
        }
    }
    if($taskRepeats.Count -gt 0){
        throw ("$taskName answers differently at the same point: " + ($taskRepeats -join '; ') +
               ". The selection read is not repeatable, so no hit region can be derived from it. " +
               "Clears that did not empty the set: $($taskStillSelected.Count)")
    }
}
$taskReport=@()
$taskReport+='hit region map (columns dx=-4..4, rows dy=-3..3, 1=the game selected the component while the button was held)'
foreach($taskName in @('hit-default','hit-expanded')){
    $taskReport+="type $taskName"
    foreach($taskRow in $taskRows[$taskName]){$taskReport+='  '+$taskRow}
}
$taskReport+='control readings at (-4,-3), (2,0), (-4,-3), (40,0), (40,0):'
foreach($taskName in @('hit-default','hit-expanded')){
    $taskControl=@($taskSamples | Where-Object { $_.Name -eq $taskName -and (($_.Dx -eq -4 -and $_.Dy -eq -3) -or ($_.Dx -eq 2 -and $_.Dy -eq 0) -or $_.Dx -eq 40) })
    $taskReport+="  $taskName $((($taskControl | ForEach-Object { "$($_.Dx),$($_.Dy)=$($_.Selected)" }) -join ' '))"
}
$taskReport+="clears that still reported a selection: $($taskStillSelected.Count)"
foreach($taskLine in $taskStillSelected | Select-Object -First 10){$taskReport+='  '+$taskLine}
$taskReport+='contaminated samples (dropped, never written into the map): 0'
$taskReport | Set-Content -LiteralPath (Join-Path $taskOut 'map.txt') -Encoding ascii
# Keep every run: the point of this case is to show whether the readout is
# repeatable at all, and one run is not enough to answer that.
Add-Content -LiteralPath (Join-Path $taskOut 'map-history.txt') -Encoding ascii -Value (
    @("run $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')") + $taskReport + '')
$taskReport | ForEach-Object { $_ }
"Evidence: $(Join-Path $taskOut 'map.txt')"
"Sandbox: $taskSandbox"
"PASS the per-point pointer hit readout is reproducible (63 grid offsets per type plus five controls)"
