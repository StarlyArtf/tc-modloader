# Runs the native custom-logic example in an isolated game copy.  The example
# imports a component whose internal circuit is AND and registers a C++ OR
# callback for it, so the level result can only come from the callback.
#
# Every scenario loads a real campaign level, lets the game compile the board,
# judge the outputs and drive the reset, then asserts on the loader log.
param(
  [ValidateSet('or','mixed','multi','shape1','shape3','shape32','shapew8','shape_xor8','shape_mux8','shape_asr8','shape_adder8')]
  [string]$Scenario = 'or'
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskMod = Join-Path $taskRepo 'dist\example.custom-or.mod'
if(!(Test-Path -LiteralPath $taskMod)) { throw 'Missing dist\example.custom-or.mod; run build.ps1 first' }

# Scenario table: schematic, campaign level, and the log evidence that only a
# working callback-in-the-real-simulation can produce.
$taskScenarios = @{
  or = @{
    Schematic = 'build\nl_single.data'; Level = 'or_gate'
    Definition = $null
    LevelInputs = 1; LevelOutputs = 1
    Detail = 'level input -> Mod component -> level output; internal AND, callback OR'
    Expect = @(
      @{Pattern='Native logic: registered custom 0x414e44325f303031 inputs=2 outputs=1 .*shape=2in/1out';Why='shape and pin geometry registered'},
      @{Pattern='custom-or: cycle=0 instance=0x[0-9a-f]+ in=\[0,0\] out=\[0\] calls=1';Why='cycle 0 OR'},
      @{Pattern='custom-or: cycle=1 instance=0x[0-9a-f]+ in=\[1,0\] out=\[1\] calls=2';Why='cycle 1 OR'},
      @{Pattern='custom-or: cycle=2 instance=0x[0-9a-f]+ in=\[0,1\] out=\[1\] calls=3';Why='cycle 2 OR'},
      @{Pattern='custom-or: cycle=3 instance=0x[0-9a-f]+ in=\[1,1\] out=\[1\] calls=4';Why='cycle 3 OR'},
      @{Pattern='custom-or: run finished cycle=3 callback_calls=4 instances=1 verdict=1';Why='one callback instance, one call per cycle, level won'},
      @{Pattern='custom-or: game test result=1';Why='the game rated the OR truth table as win'},
      @{Pattern='custom-or: game output history cycle=0 z=0 value=0';Why='level pin history cycle 0'},
      @{Pattern='custom-or: game output history cycle=1 z=0 value=1';Why='level pin history cycle 1'},
      @{Pattern='custom-or: game output history cycle=2 z=0 value=1';Why='level pin history cycle 2'},
      @{Pattern='custom-or: game output history cycle=3 z=0 value=1';Why='level pin history cycle 3'},
      @{Pattern='custom-or: after refresh peeks=[1-9][0-9]* callback_calls=4';Why='refresh ran without committing state'},
      @{Pattern='Native logic: reset 1 instance\(s\)';Why='loader reset reached the callback'},
      @{Pattern='custom-or: reset instance=0x[0-9a-f]+ calls=4 persisted_state=0';Why='per-instance state survived until reset'},
      @{Pattern='custom-or: after reset resets=[1-9][0-9]*';Why='reset callback reached the instance'},
      @{Pattern='custom-or: run after reset cycle=1 callback_calls=6';Why='the callback keeps running after reset'}
    )
  }
  mixed = @{
    Schematic = 'build\nl_mixed.data'; Level = 'nor_gate'
    Definition = $null
    LevelInputs = 1; LevelOutputs = 1
    Detail = 'input -> Mod component (OR callback) -> native NOT -> output; the NOR level only passes when both run'
    Expect = @(
      @{Pattern='custom-or: cycle=0 instance=0x[0-9a-f]+ in=\[0,0\] out=\[0\] calls=1';Why='callback still drives the component'},
      @{Pattern='custom-or: cycle=3 instance=0x[0-9a-f]+ in=\[1,1\] out=\[1\] calls=4';Why='OR result of the callback'},
      @{Pattern='custom-or: run finished cycle=3 callback_calls=4 instances=1 verdict=1';Why='one callback instance, level won'},
      @{Pattern='custom-or: game test result=1';Why='native NOT inverts the callback result and the game passes the NOR level'},
      @{Pattern='custom-or: game output history cycle=0 z=0 value=1';Why='native NOT output cycle 0'},
      @{Pattern='custom-or: game output history cycle=1 z=0 value=0';Why='native NOT output cycle 1'},
      @{Pattern='custom-or: game output history cycle=2 z=0 value=0';Why='native NOT output cycle 2'},
      @{Pattern='custom-or: game output history cycle=3 z=0 value=0';Why='native NOT output cycle 3'}
    )
  }
  multi = @{
    Schematic = 'build\nl_multi.data'; Level = 'or_gate'
    Definition = $null
    LevelInputs = 1; LevelOutputs = 1
    Detail = 'two Mod instances of the same definition feed one native AND'
    Expect = @(
      @{Pattern='Native logic: bound instance 0x[0-9a-f]+ of custom 0x414e44325f303031 as token 1';Why='first instance binding'},
      @{Pattern='Native logic: bound instance 0x[0-9a-f]+ of custom 0x414e44325f303031 as token 2';Why='second instance binding'},
      @{Pattern='custom-or: cycle=1 instance=0x5555555555555555 in=\[1,0\] out=\[1\] calls=2';Why='second instance runs its own state'},
      @{Pattern='custom-or: cycle=1 instance=0x2222222222222222 in=\[1,0\] out=\[1\] calls=2';Why='first instance runs its own state'},
      @{Pattern='custom-or: run finished cycle=3 callback_calls=8 instances=2 verdict=1';Why='each instance called once per cycle, level won'},
      @{Pattern='custom-or: game test result=1';Why='native AND of two Mod instances passes the OR level'},
      @{Pattern='custom-or: game output history cycle=3 z=0 value=1';Why='level pin history cycle 3'},
      @{Pattern='custom-or: after reset resets=[2-9]';Why='both instances were reset'}
    )
  }
  shape1 = @{
    Schematic = 'build\nl_not1board.data'; Level = 'not_gate'
    Definition = @('nl_def_not1.data')
    LevelInputs = 1; LevelOutputs = 1
    Detail = 'one-input one-output Mod component (NOT callback) on the NOT Gate level'
    Expect = @(
      @{Pattern='Native logic: registered custom 0x4e4f54315f303031 inputs=1 outputs=1 .*shape=1in/1out';Why='one-input shape registered'},
      @{Pattern='custom-or: cycle=0 instance=0x[0-9a-f]+ in=\[0\] out=\[1\] calls=1';Why='NOT of input 0'},
      @{Pattern='custom-or: cycle=1 instance=0x[0-9a-f]+ in=\[1\] out=\[0\] calls=2';Why='NOT of input 1'},
      @{Pattern='custom-or: run finished cycle=1 .*instances=1 verdict=1';Why='two-row NOT level won'},
      @{Pattern='custom-or: game test result=1';Why='the one-input callback satisfies the NOT level'},
      @{Pattern='custom-or: game output history cycle=0 z=0 value=1';Why='level pin history cycle 0'},
      @{Pattern='custom-or: game output history cycle=1 z=0 value=0';Why='level pin history cycle 1'}
    )
  }
  shape3 = @{
    Schematic = 'build\nl_and3board.data'; Level = 'and_gate_3'
    Definition = @('nl_def_and3.data')
    LevelInputs = 1; LevelOutputs = 1
    Detail = 'three-input one-output Mod component (AND callback) on the three-input AND level'
    Expect = @(
      @{Pattern='Native logic: registered custom 0x414e44335f303031 inputs=3 outputs=1 in0=\(-2,-1,w1\) in1=\(-2,0,w1\) in2=\(-2,1,w1\) out0=\(2,0,w1\) shape=3in/1out';Why='three-input shape and pin geometry registered'},
      @{Pattern='custom-or: cycle=1 instance=0x[0-9a-f]+ in=\[1,0,0\] out=\[0\] calls=2';Why='input tuple keeps pin order'},
      @{Pattern='custom-or: cycle=6 instance=0x[0-9a-f]+ in=\[0,1,1\] out=\[0\] calls=7';Why='two of three inputs'},
      @{Pattern='custom-or: cycle=7 instance=0x[0-9a-f]+ in=\[1,1,1\] out=\[1\] calls=8';Why='all three inputs high'},
      @{Pattern='custom-or: run finished cycle=7 callback_calls=8 instances=1 verdict=1';Why='game judged the three-input level as win'},
      @{Pattern='custom-or: game output history cycle=0 z=0 value=0';Why='level pin history cycle 0'},
      @{Pattern='custom-or: game output history cycle=7 z=0 value=1';Why='level pin history cycle 7'}
    )
  }
  shape32 = @{
    Schematic = 'build\nl_adderboard.data'; Level = 'full_adder'
    Definition = @('nl_def_adder.data')
    LevelInputs = 3; LevelOutputs = 2
    Detail = 'three-input two-output Mod component (full adder callback) on the full adder level'
    Expect = @(
      @{Pattern='Native logic: registered custom 0x414444525f303031 inputs=3 outputs=2 .*shape=3in/2out';Why='three-input two-output shape registered'},
      @{Pattern='custom-or: cycle=1 instance=0x[0-9a-f]+ in=\[1,0,0\] out=\[1,0\] calls=2';Why='sum and carry of one input'},
      @{Pattern='custom-or: cycle=3 instance=0x[0-9a-f]+ in=\[1,1,0\] out=\[0,1\] calls=4';Why='sum and carry of two inputs'},
      @{Pattern='custom-or: cycle=7 instance=0x[0-9a-f]+ in=\[1,1,1\] out=\[1,1\] calls=8';Why='sum and carry of three inputs'},
      @{Pattern='custom-or: run finished cycle=7 callback_calls=8 instances=1 verdict=1';Why='game checked both output pins and won'},
      @{Pattern='custom-or: game output history cycle=1 z=0 value=1';Why='sum history cycle 1'},
      @{Pattern='custom-or: game output history cycle=3 z=0 value=0';Why='sum history cycle 3'},
      @{Pattern='custom-or: game output history cycle=3 pin=1 z=0 value=1';Why='carry history cycle 3'},
      @{Pattern='custom-or: game output history cycle=7 pin=1 z=0 value=1';Why='carry history cycle 7'}
    )
  }
  shapew8 = @{
    Schematic = 'build\nl_double8board.data'; Level = 'double_number'
    Definition = @('nl_def_double8.data')
    LevelInputs = 1; LevelOutputs = 1
    Detail = 'one 8-bit input and one 8-bit output Mod component on the double-number level'
    Expect = @(
      @{Pattern='Native logic: registered custom 0x44424c385f303031 inputs=1 outputs=1 in0=\(-2,0,w8\) out0=\(2,0,w8\) shape=1in/1out';Why='word-width shape registered'},
      @{Pattern='custom-or: cycle=0 instance=0x[0-9a-f]+ in=\[[0-9]+\] out=\[[0-9]+\] calls=1';Why='whole byte reaches the callback'},
      @{Pattern='custom-or: cycle=15 instance=0x[0-9a-f]+ in=\[[0-9]+\] out=\[[0-9]+\] calls=16';Why='16 cycles of word values'},
      @{Pattern='custom-or: run finished cycle=15 callback_calls=16 instances=1 verdict=1';Why='the game compared every byte and won'},
      # Word-width level pins do not write the one-bit pin history buffer, so the
      # game-side evidence is the level's own table text.
      @{Pattern='custom-or: level text offset=50000 value=\[right\][0-9]+\[/right\]';Why='level displayed an expected byte'},
      @{Pattern='custom-or: level text offset=80000 value=\[right\][0-9]+\[/right\]';Why='level displayed the byte produced by the callback'}
    )
  }
  shape_xor8 = @{
    Schematic = 'build\nl_xor8board.data'; Level = 'byte_xor'
    Definition = @('nl_def_xor8.data')
    LevelInputs = 2; LevelOutputs = 1; Cycles = 40
    Detail = 'two 8-bit inputs -> 8-bit output (XOR), checked by the level for 40 cycles'
    Check = { param($i,$o) ($i[0] -bxor $i[1]) -eq $o[0] }
    Expect = @(
      @{Pattern='Native logic: registered custom 0x584f52385f303031 inputs=2 outputs=1 in0=\(-2,0,w8\) in1=\(-2,1,w8\) out0=\(2,0,w8\)';Why='two word inputs registered'},
      @{Pattern='custom-or: run finished cycle=40 callback_calls=41 instances=1 verdict=0';Why='the game found no mismatch in 40 cycles'}
    )
  }
  shape_mux8 = @{
    Schematic = 'build\nl_mux8board.data'; Level = 'byte_mux'
    Definition = @('nl_def_mux8.data')
    LevelInputs = 3; LevelOutputs = 1; Cycles = 40
    Detail = 'three 8-bit inputs -> 8-bit output (mux), checked by the level for 40 cycles'
    # PowerShell 5.1 has no ternary operator: write the select-the-B-when-set
    # choice as an if expression (this file is run by powershell.exe, not pwsh).
    Check = { param($i,$o) $(if ($i[0] -eq 1) { $i[2] } else { $i[1] }) -eq $o[0] }
    Expect = @(
      @{Pattern='Native logic: registered custom 0x4d5558385f303031 inputs=3 outputs=1 in0=\(-2,-1,w8\) in1=\(-2,0,w8\) in2=\(-2,1,w8\) out0=\(2,0,w8\)';Why='three word inputs registered'},
      @{Pattern='custom-or: run finished cycle=40 callback_calls=41 instances=1 verdict=0';Why='the game found no mismatch in 40 cycles'}
    )
  }
  shape_asr8 = @{
    Schematic = 'build\nl_asr8board.data'; Level = 'byte_asr'
    Definition = @('nl_def_asr8.data')
    LevelInputs = 2; LevelOutputs = 1; Cycles = 40
    Detail = '8-bit value plus 3-bit shift -> 8-bit arithmetic shift, checked for 40 cycles'
    Check = {
      param($i,$o)
      $taskValue = [int]$i[0] -band 0xff
      if($taskValue -ge 128) { $taskValue -= 256 }
      $taskExpected = ($taskValue -shr ([int]$i[1] -band 7)) -band 0xff
      $taskExpected -eq ([int]$o[0] -band 0xff)
    }
    Expect = @(
      @{Pattern='Native logic: registered custom 0x415352385f303031 inputs=2 outputs=1 in0=\(-2,0,w8\) in1=\(-2,1,w3\) out0=\(2,0,w8\)';Why='mixed 8-bit and 3-bit inputs registered'},
      @{Pattern='custom-or: run finished cycle=40 callback_calls=41 instances=1 verdict=0';Why='the game found no mismatch in 40 cycles'}
    )
  }
  shape_adder8 = @{
    Schematic = 'build\nl_adder8board.data'; Level = 'byte_adder'
    Definition = @('nl_def_adder8.data')
    LevelInputs = 3; LevelOutputs = 2; Cycles = 40
    Detail = 'carry in + two bytes -> byte sum + carry out, checked for 40 cycles'
    Check = { param($i,$o) (($i[0] -band 1) + ($i[1] -band 0xff) + ($i[2] -band 0xff)) -eq (($o[1] -band 1) * 256 + ($o[0] -band 0xff)) }
    Expect = @(
      @{Pattern='Native logic: registered custom 0x414444385f303031 inputs=3 outputs=2 in0=\(-2,-1,w1\) in1=\(-2,0,w8\) in2=\(-2,1,w8\) out0=\(2,0,w8\) out1=\(2,1,w1\)';Why='mixed 1-bit and 8-bit pins registered on both directions'},
      @{Pattern='custom-or: run finished cycle=40 callback_calls=41 instances=1 verdict=0';Why='the game checked both output pins for 40 cycles without a mismatch'}
    )
  }
}
if(!$taskScenarios.ContainsKey($Scenario)) { throw "Unknown scenario $Scenario" }
$taskCase = $taskScenarios[$Scenario]
$taskSchematic = Join-Path $taskRepo $taskCase.Schematic
if(!(Test-Path -LiteralPath $taskSchematic)) { throw "Missing $taskSchematic; run build.ps1 first" }

$taskTest = Join-Path $taskRepo ('build\custom-or-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.custom-or'
$taskSchema = Join-Path $taskProfile ('AppData\Roaming\Turing Complete Mods\profiles\default\schematics\' + $taskCase.Level + '\Default')
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods'),$taskData,$taskSchema | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath $taskMod -Destination (Join-Path $taskRoot 'mods\example.custom-or.mod')
Copy-Item -LiteralPath $taskSchematic -Destination (Join-Path $taskSchema 'circuit.data')
# The optional second line caps how many cycles the autotest runs.  Levels whose
# own test only wins after thousands of cycles are judged by "no mismatch yet".
if($taskCase.Cycles) {
  Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value @($taskCase.Level,[string]$taskCase.Cycles) -Encoding ascii
} else {
  Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value $taskCase.Level -Encoding ascii
}
# The plugin also registers extra interface shapes; only the definition the
# scenario needs is staged, because a rejected import leaves the game's Nim
# error flag set and would block later ones.
foreach($taskDefinition in @($taskCase.Definition)) {
  if(!$taskDefinition) { continue }
  $taskShapePath = Join-Path $taskRepo ('build\' + $taskDefinition)
  if(!(Test-Path -LiteralPath $taskShapePath)) { throw "Missing $taskShapePath; run build.ps1 first" }
  Copy-Item -LiteralPath $taskShapePath -Destination (Join-Path $taskData $taskDefinition)
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply example.custom-or
if($LASTEXITCODE){throw 'Custom OR package apply failed'}

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -RedirectStandardOutput (Join-Path $taskTest "stdout.txt") -RedirectStandardError (Join-Path $taskTest "stderr.txt") -PassThru
  Start-Sleep -Seconds 45
  if(!$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
}

$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
if(!(Test-Path -LiteralPath $taskLog)) { throw "Missing loader log; inspect $taskTest" }
$taskText = Get-Content -LiteralPath $taskLog -Raw
foreach($taskExpect in $taskCase.Expect) {
  if(![regex]::IsMatch($taskText,$taskExpect.Pattern)) {
    throw ("Scenario {0}: missing evidence '{1}' ({2}); inspect {3}" -f $Scenario,$taskExpect.Pattern,$taskExpect.Why,$taskLog)
  }
}
if(!(Select-String -LiteralPath $taskLog -Pattern 'custom-or: autotest finished' -Quiet)) {
  throw "Scenario $Scenario did not finish; inspect $taskLog"
}

# The compiled board must carry exactly the level's own IO components.  A
# second output component would let the level test read one pin while the
# in-game table shows another one.
$taskSource = Join-Path $taskRoot 'native-logic-source.txt'
if(!(Test-Path -LiteralPath $taskSource)) { throw "Missing compiled source dump; inspect $taskTest" }
$taskSourceText = Get-Content -LiteralPath $taskSource -Raw
# Each node appears once per generated mode, so count distinct node indices.
$taskNodes = @{}
foreach($taskMatch in [regex]::Matches($taskSourceText,'//\s*(\d+)\s+(com_level_(?:input|output)_[a-z0-9_]+)\b')) {
  $taskNodes[$taskMatch.Groups[2].Value + '#' + $taskMatch.Groups[1].Value] = $true
}
foreach($taskKind in @(@{Name='input';Expect=$taskCase.LevelInputs},@{Name='output';Expect=$taskCase.LevelOutputs})) {
  $taskFound = @($taskNodes.Keys | Where-Object { $_ -like ('com_level_' + $taskKind.Name + '_*') }).Count
  if($taskFound -ne $taskKind.Expect) {
    throw ("Scenario {0}: compiled board has {1} level {2} node(s), expected {3}; inspect {4}" -f $Scenario,$taskFound,$taskKind.Name,$taskKind.Expect,$taskSource)
  }
}
if([regex]::IsMatch($taskSourceText,'level_output\.[a-z0-9_]*is_z = true')) {
  throw ("Scenario {0}: a level output pin stayed high-impedance; inspect {1}" -f $Scenario,$taskSource)
}

# Independent check of the values the bridge exchanged: the callback's own log
# lines carry the raw words, so the intended function is verified outside the
# plugin and the game's verdict.
if($taskCase.Check) {
  $taskSamples = [regex]::Matches($taskText,'custom-or: cycle=(\d+) instance=0x[0-9a-f]+ in=\[([0-9,\s]+)\] out=\[([0-9,\s]+)\]')
  $taskChecked = 0
  foreach($taskSample in $taskSamples) {
    $taskIn = @($taskSample.Groups[2].Value.Split(',') | ForEach-Object { [int]$_ })
    $taskOut = @($taskSample.Groups[3].Value.Split(',') | ForEach-Object { [int]$_ })
    if(-not (& $taskCase.Check $taskIn $taskOut)) {
      throw ("Scenario {0}: cycle {1} exchanged in=[{2}] out=[{3}], which does not match the intended function" -f $Scenario,$taskSample.Groups[1].Value,($taskIn -join ','),($taskOut -join ','))
    }
    $taskChecked++
  }
  if($taskChecked -lt 10) { throw ("Scenario {0}: only {1} callback samples to check; inspect {2}" -f $Scenario,$taskChecked,$taskLog) }
  "Checked $taskChecked word samples against the intended function"
}

"PASS scenario $Scenario ($($taskCase.Detail)) on level $($taskCase.Level)"
$taskGeometry = Select-String -LiteralPath $taskLog -Pattern 'Native logic: registered' | ForEach-Object { $_.Line } | Select-Object -First 1
"Evidence: $taskGeometry"
"Sandbox: $taskTest"
