# End-to-end check of the real example.byte-adder package in an isolated game
# copy: the campaign Byte Adder level drives carry_in + A + B and the level
# itself compares (carry_out << 8) | sum.  Both callback phases are asserted,
# because the on-screen table reads the refresh path while the level test reads
# the simulated cycles.
param([string]$PackagePath, [string]$SchematicPath, [switch]$Stress, [switch]$Single)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskSchematic = Join-Path $taskRepo 'build\nl_adder8board.data'
if($SchematicPath) { $taskSchematic = $SchematicPath }
if(!$PackagePath) { $PackagePath = Join-Path $taskRepo 'dist\example.byte-adder.mod' }
if(!(Test-Path -LiteralPath $taskSchematic)) { throw "Missing $taskSchematic; run build.ps1 first" }

$taskTest = Join-Path $taskRepo ('build\byte-adder-smoke-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.byte-adder'
$taskLevel = if($Single) { 'double_number' } else { 'byte_adder' }
$taskSchema = Join-Path $taskProfile "AppData\Roaming\Turing Complete Mods\profiles\default\schematics\$taskLevel\Default"
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods'),$taskData,$taskSchema | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath $PackagePath -Destination (Join-Path $taskRoot 'mods\example.byte-adder.mod')
Copy-Item -LiteralPath $taskSchematic -Destination (Join-Path $taskSchema 'circuit.data')
Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value $taskLevel -Encoding ascii
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply example.byte-adder
if($LASTEXITCODE){throw 'Byte adder package apply failed'}

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds 45
  if(!$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
}

$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
if(!(Test-Path -LiteralPath $taskLog)) { throw "Missing loader log; inspect $taskTest" }
$taskText = Get-Content -LiteralPath $taskLog -Raw
if($Single) {
  if($taskText -notmatch 'inputs=1 outputs=1' -or $taskText -notmatch 'autotest finished cycle=15 verdict=1') {
    throw "Single-pin declarative component did not win; inspect $taskLog"
  }
  $taskSamples = [regex]::Matches($taskText,'declarative: double input=(\d+) output=(\d+)')
  if($taskSamples.Count -lt 16) { throw "Too few single-pin samples; inspect $taskLog" }
  foreach($taskSample in $taskSamples) {
    if([int]$taskSample.Groups[2].Value -ne (([int]$taskSample.Groups[1].Value * 2) -band 255)) {
      throw 'Single-pin calculation mismatch'
    }
  }
  "PASS declarative single-pin byte component: level won, $($taskSamples.Count) samples checked"
  "Sandbox: $taskTest"
  return
}

# The component must be bridged: three inputs, two outputs, mixed widths.
$taskGeometry = 'Native logic: registered custom 0x414444385f303031 inputs=3 outputs=2 in0=\(-2,-1,w1\) in1=\(-2,0,w8\) in2=\(-2,1,w8\) out0=\(2,0,w8\) out1=\(2,1,w1\)'
if($Stress) { $taskGeometry = 'Native logic: registered custom 0x414444385f303031 inputs=8 outputs=8' }
if($Stress -and ($taskText -notmatch 'declarative: duplicate and invalid descriptors rejected' -or
   $taskText -match 'order=0' -or $taskText -notmatch 'declarative: inputs=1,130,4,1,130,1,130,4, order=1')) {
  throw "Stress registration/order evidence missing; inspect $taskLog"
}
if(![regex]::IsMatch($taskText,$taskGeometry)) {
  throw "Byte adder geometry was not registered; inspect $taskLog"
}
if(!(Select-String -LiteralPath $taskLog -Pattern 'byte-adder: autotest finished cycle=39 verdict=0' -Quiet)) {
  throw "The level test did not finish without a mismatch; inspect $taskLog"
}

# Refresh (UI) phase: the example must compute the outputs there too, otherwise
# the displayed sum/carry stay at whatever the last reset left behind.
$taskPeeks = [regex]::Matches($taskText,'byte-adder: peek carry_in=(\d+) a=(\d+) b=(\d+) sum=(\d+) carry_out=(\d+)')
if($taskPeeks.Count -lt 1) {
  throw "No refresh-phase callback ran; the element cannot display a result; inspect $taskLog"
}
foreach($taskPeek in $taskPeeks) {
  $taskCarry = [int]$taskPeek.Groups[1].Value
  $taskA = [int]$taskPeek.Groups[2].Value
  $taskB = [int]$taskPeek.Groups[3].Value
  $taskSum = [int]$taskPeek.Groups[4].Value
  $taskCarryOut = [int]$taskPeek.Groups[5].Value
  $taskTotal = $taskCarry + $taskA + $taskB
  if($taskSum -ne ($taskTotal -band 0xff) -or $taskCarryOut -ne (($taskTotal -shr 8) -band 1)) {
    throw "Refresh exchanged carry_in=$taskCarry a=$taskA b=$taskB sum=$taskSum carry_out=$taskCarryOut, which is not the intended sum"
  }
}
"Checked $($taskPeeks.Count) refresh sample(s) against the intended sum"

# Simulated cycles: same function, verified from the callback's raw words.
$taskSamples = [regex]::Matches($taskText,'byte-adder: cycle=(\d+) carry_in=(\d+) a=(\d+) b=(\d+) sum=(\d+) carry_out=(\d+)')
$taskChecked = 0
foreach($taskSample in $taskSamples) {
  $taskCarry = [int]$taskSample.Groups[2].Value
  $taskA = [int]$taskSample.Groups[3].Value
  $taskB = [int]$taskSample.Groups[4].Value
  $taskSum = [int]$taskSample.Groups[5].Value
  $taskCarryOut = [int]$taskSample.Groups[6].Value
  $taskTotal = $taskCarry + $taskA + $taskB
  if($taskSum -ne ($taskTotal -band 0xff) -or $taskCarryOut -ne (($taskTotal -shr 8) -band 1)) {
    throw "Cycle $($taskSample.Groups[1].Value) exchanged carry_in=$taskCarry a=$taskA b=$taskB sum=$taskSum carry_out=$taskCarryOut, which is not the intended sum"
  }
  $taskChecked++
}
if($taskChecked -lt 8) { throw "Only $taskChecked cycle samples to check; inspect $taskLog" }
"Checked $taskChecked cycle sample(s) against the intended sum"

"PASS example.byte-adder: refresh and simulated cycles both compute carry_in + A + B"
if($Stress) { 'PASS declarative 8 inputs / 8 outputs: order checked, level reads outputs 6 and 7' }
"Sandbox: $taskTest"
