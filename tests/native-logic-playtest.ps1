# Runs every native logic callback scenario in its own isolated game copy.
# Each scenario asserts on evidence produced by the game itself: the level
# verdict, the level output pin history, per-instance callback counters, and
# the level IO components present in the compiled board.
$ErrorActionPreference = 'Stop'
foreach($taskScenario in @('or','mixed','multi','shape1','shape3','shape32','shapew8','shape_xor8','shape_mux8','shape_asr8','shape_adder8')) {
  & (Join-Path $PSScriptRoot 'custom-or-playtest.ps1') -Scenario $taskScenario
  if($LASTEXITCODE) { throw "Native logic scenario $taskScenario failed" }
}
'PASS native logic callbacks: single, mixed with a native gate, two instances, 1in/1out, 3in/1out, 3in/2out, 8-bit word, 2x8-bit, 3x8-bit, 8+3-bit, 1+8+8 -> 8+1 bit'
