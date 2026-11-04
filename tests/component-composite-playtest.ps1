# Real-machine gate for the internal-netlist builder (`tc.component.compose` V2,
# M4's second cut): a composite type whose behaviour is the game's own circuit.
#
# The probe (tests/composite-probe.cpp) registers a three-input AND built from two
# of the game's own two-input AND parts, nests one composite inside another, and
# puts the first one in the path of a real campaign level: the fixture board
# build/nl_and3board.data wires the and_gate_3 level's three inputs and its output
# to an instance of the type, and the level's own test only passes when the truth
# table is right.  So the run proves three separate things:
#
#   * the game accepts a definition the builder encoded (import + prototype);
#   * the game **compiled the netlist** - it recomputed the gate count from the
#     two nodes (2) while the probe declared 1, and kept the declared delay;
#   * a composite behaves: the level verdict is 1;
#   * plus the refusals (a netlist with an undriven pin) and the nesting path.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskMod = Join-Path $taskRepo 'dist\dev.composite-probe.mod'
$taskBoard = Join-Path $taskRepo 'build\nl_and3board.data'
if(!(Test-Path -LiteralPath $taskMod)) { throw 'Missing dist\dev.composite-probe.mod; run build.ps1 first' }
if(!(Test-Path -LiteralPath $taskBoard)) { throw "Missing $taskBoard; run build.ps1 first" }

$taskTest = Join-Path $taskRepo ('build\component-composite-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.composite-probe'
$taskSchema = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default\schematics\and_gate_3\Default'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods'),$taskData,$taskSchema | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath $taskMod -Destination (Join-Path $taskRoot 'mods\dev.composite-probe.mod')
# The level takes its board from this file; the probe supplies the type.
Copy-Item -LiteralPath $taskBoard -Destination (Join-Path $taskSchema 'circuit.data')
# The byte-adder runner the probe reuses reads the level name from here.
Set-Content -LiteralPath (Join-Path $taskData 'autotest.txt') -Value 'and_gate_3' -Encoding ascii
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.composite-probe
if($LASTEXITCODE){throw 'Composite probe package apply failed'}

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskProcess = $null
$taskResult = Join-Path $taskData 'result.txt'
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds(60)
  while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
    Start-Sleep -Milliseconds 250
  }
  if(!(Test-Path -LiteralPath $taskResult)) {
    throw "Composite probe did not finish; inspect $taskRoot\tc-modloader-data\loader.log"
  }
  $taskText = [IO.File]::ReadAllText($taskResult)
  $taskText
  if(!$taskText.StartsWith('PASS ')) { throw "Composite probe failed: $taskText" }
  $taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
  $taskLines = Get-Content -LiteralPath $taskLog
  foreach($taskExpected in @(
      # The builder imported both composites through the game's own component model.
      'Component compose: composite 0x414e44335f303031 imported from 2 node\(s\), 3 in/1 out, declared delay=2',
      'Component compose: composite 0x4e455354335f3031 imported from 1 node\(s\), 3 in/1 out, declared delay=2',
      # The game's own read of the prototype: the pins are the ones declared, the
      # gate count is the one it recomputed from the two AND nodes (2, not the
      # declared 1), and the declared delay survived.
      'composite: and3 custom=0x414e44335f303031 in=3 out=1 gates=2 delay=2',
      'composite: nested custom=0x4e455354335f3031 in=3 out=1 gates=[1-9][0-9]* delay=2',
      # The catalogue knows it as a composite.
      'composite: catalogue custom=0x414e44335f303031 implementation=1 capabilities=\d+ active=1 in=3 out=1',
      # A wide type: two inputs, thirty-two outputs, and a sixteen-cell pin lane -
      # the game reads the pins back where the lane puts them.
      'composite: wide lane=0',
      # The game's own read of where the lane put the pins: 31 cells apart
      # instead of the default 4 (the input side lands one cell inward - that is
      # the game's rounding of the pin's own connection offset, and the board
      # read reports the same number, so a wire lands where the game says).
      'composite: wide custom=0x57494445355f3031 in=2 out=32 gates=0 delay=1 in0=\(-15,0\) out0=\(16,-15\)',
      # The refusals: an undriven input is refused before anything is imported.
      'composite: refusals broken=1 status=an inner input pin is not driven: node builtin/AND pin 1 has no wire driving it',
      # Cut three, path one: the metadata clone.  The game's own prototype read
      # shows the name and description it now stores, and the loader's catalogue
      # follows.
      'composite: clone status=0',
      'composite: clone prototype name="AND" description="[^"]+"',
      'composite: clone catalogue name="AND" was="Composite AND3 \(nested\)"',
      # Cut three, path three: the full alias, refused explicitly (-9) instead of
      # registering a look-alike shell.
      'composite: alias status=-9',
      # The game's own level test judged the composite's truth table.
      'byte-adder: autotest finished cycle=\d+ verdict=1',
      'composite: level verdict=1')){
    if(!($taskLines | Where-Object { $_ -match $taskExpected })){
      $taskLines | Where-Object { $_ -match 'composite:|byte-adder: autotest' } | Select-Object -Last 12
      throw "Missing composite evidence: $taskExpected"
    }
  }
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  if($taskProcess -and !$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
  }
  "Evidence: $taskTest"
}
