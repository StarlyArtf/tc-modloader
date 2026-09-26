# Turns a `state-map.txt` from the dev.sim-state probe into a small board
# fixture the offline netlist test can read (tests/data/board-*.txt).
#
# Why a fixture at all: the sandbox simulator has to build its netlist from the
# game's own board records, and the only honest way to test that builder without
# a game is to feed it records a real game produced.  The probe already dumps
# them; this script keeps the fields the builder reads so the regression stays
# small enough to review.
#
#   powershell -File tools/extract-board-fixture.ps1 -StateMap build/state-map.txt -Output tests/data/and_gate-board.txt
param(
  [Parameter(Mandatory = $true)][string]$StateMap,
  [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'

# Little-endian u64 out of a hex dump, which is how the game stores every word
# these records carry.
function Read-LeU64([string]$Hex, [int]$Offset) {
  $taskBytes = New-Object byte[] 8
  for ($taskIndex = 0; $taskIndex -lt 8; $taskIndex++) {
    $taskBytes[$taskIndex] = [Convert]::ToByte($Hex.Substring(($Offset + $taskIndex) * 2, 2), 16)
  }
  return [BitConverter]::ToUInt64($taskBytes, 0)
}

$taskLines = Get-Content -LiteralPath $StateMap
$taskOut = New-Object System.Collections.Generic.List[string]
$taskOut.Add('# Board records captured from a real session (tests/sim-state-probe.cpp,')
$taskOut.Add('# dev.sim-state package).  Regenerate with:')
$taskOut.Add('#   powershell -File tools/extract-board-fixture.ps1 -StateMap build/state-map.txt -Output tests/data/<name>.txt')
$taskOut.Add('# component: kind/x/y/rotation/id/custom from the record, plus the four words the probe')
$taskOut.Add('# decodes around it.  wire: both endpoints, the width and the state slot the game itself reads.')
$taskComponents = @{}
$taskWords = @{}
$taskBoardLine = ''
foreach ($taskLine in $taskLines) {
  if ($taskLine -like 'board components=*') { $taskBoardLine = 'board ' + $taskLine; continue }
  if ($taskLine -like 'board_component *') {
    $taskMatch = [regex]::Match($taskLine, '^board_component (\d+) kind=0x([0-9a-f]+) pos=(-?\d+),(-?\d+) id=(\d+) bytes=([0-9a-f]+)$')
    if (!$taskMatch.Success) { throw "Unparsed component line: $taskLine" }
    $taskComponents[[int]$taskMatch.Groups[1].Value] = @{
      kind = $taskMatch.Groups[2].Value
      x = $taskMatch.Groups[3].Value
      y = $taskMatch.Groups[4].Value
      id = $taskMatch.Groups[5].Value
    }
    continue
  }
  if ($taskLine -like 'deep_component * bytes=*') {
    $taskMatch = [regex]::Match($taskLine, '^deep_component (\d+) bytes=([0-9a-f]+)$')
    if (!$taskMatch.Success) { throw "Unparsed deep component line: $taskLine" }
    $taskBytes = $taskMatch.Groups[2].Value
    if ($taskBytes.Length -lt 0x1a0 * 2) {
      throw "Component $($taskMatch.Groups[1].Value) dumped only $($taskBytes.Length / 2) bytes; rerun the probe with TC_SIM_STATE_DEEP=1"
    }
    $taskComponents[[int]$taskMatch.Groups[1].Value].rotation = [Convert]::ToInt32($taskBytes.Substring(0x06 * 2, 2), 16)
    $taskComponents[[int]$taskMatch.Groups[1].Value].custom = Read-LeU64 $taskBytes 0x188
    continue
  }
  if ($taskLine -like 'deep_component * word@*') {
    $taskMatch = [regex]::Match($taskLine, '^deep_component (\d+) word@0x([0-9a-f]+)=(\d+)( pointee=([0-9a-f]*))?$')
    if (!$taskMatch.Success) { throw "Unparsed deep word line: $taskLine" }
    if (!$taskWords.ContainsKey([int]$taskMatch.Groups[1].Value)) {
      $taskWords[[int]$taskMatch.Groups[1].Value] = @{}
    }
    $taskWords[[int]$taskMatch.Groups[1].Value]['0x' + $taskMatch.Groups[2].Value] = $taskMatch.Groups[3].Value
    continue
  }
  if ($taskLine -like 'deep_wire *') {
    $taskMatch = [regex]::Match($taskLine, '^deep_wire (\d+) from=(-?\d+),(-?\d+) to=(-?\d+),(-?\d+) width=(\d+) slot=(\d+)$')
    if (!$taskMatch.Success) { throw "Unparsed wire line: $taskLine" }
    $taskOut.Add(("wire index={0} from={1},{2} to={3},{4} width={5} slot={6}" -f `
                  $taskMatch.Groups[1].Value, $taskMatch.Groups[2].Value, $taskMatch.Groups[3].Value, `
                  $taskMatch.Groups[4].Value, $taskMatch.Groups[5].Value, $taskMatch.Groups[6].Value, `
                  $taskMatch.Groups[7].Value))
    continue
  }
}
if ($taskBoardLine) { $taskOut.Add($taskBoardLine) }
foreach ($taskIndex in ($taskComponents.Keys | Sort-Object)) {
  $taskComponent = $taskComponents[$taskIndex]
  $taskWord = $taskWords[$taskIndex]
  $taskOut.Add(("component index={0} kind=0x{1} x={2} y={3} rotation={4} id={5} custom={6} word28={7} word30={8} word38={9} word40={10}" -f `
                $taskIndex, $taskComponent.kind, $taskComponent.x, $taskComponent.y, $taskComponent.rotation, `
                $taskComponent.id, $taskComponent.custom, $taskWord['0x28'], $taskWord['0x30'], `
                $taskWord['0x38'], $taskWord['0x40']))
}
if ($taskOut.Count -le 6) { throw "No board records found in $StateMap; rerun the probe with TC_SIM_STATE_DEEP=1" }
$taskFolder = Split-Path -Parent ([IO.Path]::GetFullPath($Output))
New-Item -ItemType Directory -Force $taskFolder | Out-Null
[IO.File]::WriteAllLines([IO.Path]::GetFullPath($Output), $taskOut, [Text.UTF8Encoding]::new($false))
"Wrote $($taskOut.Count) lines to $Output"
