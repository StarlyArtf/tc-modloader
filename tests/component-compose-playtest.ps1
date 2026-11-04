# Real-machine gate for tc.component.compose V1: the built-in type catalogue.
#
# What it proves: the service exists, answers by kind and by the stable spelling
# (`builtin/<name>`, `#2` for a name this build repeats, `builtin/0x<kind>` for
# the slots the game leaves unnamed), refuses what it should, and - the part that
# makes the numbers trustworthy - its dump agrees field by field with the dump
# the *other* reader of the same table produces.  `tests/kind-list-probe.cpp`
# walks the prototype through the SDK's TCGameModel wrapper and writes kinds.txt;
# the compose probe goes through the loader's own prototypeGet + pin decoding.
# Both run in one sandbox in the same session, so a drift in either path shows up
# as a mismatch instead of passing silently.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
foreach($taskMod in @('dev.kind-list.mod','dev.compose-probe.mod')) {
  if(!(Test-Path -LiteralPath (Join-Path $taskRepo ('dist\' + $taskMod)))) {
    throw "Missing dist\$taskMod; run build.ps1 first"
  }
}

$taskTest = Join-Path $taskRepo ('build\component-compose-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskKindData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.kind-list'
$taskComposeData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.compose-probe'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods'),$taskKindData,$taskComposeData | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
foreach($taskMod in @('dev.kind-list.mod','dev.compose-probe.mod')) {
  Copy-Item -LiteralPath (Join-Path $taskRepo ('dist\' + $taskMod)) -Destination (Join-Path $taskRoot 'mods')
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply dev.kind-list dev.compose-probe
if($LASTEXITCODE){throw 'Compose probe package apply failed'}

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskProcess = $null
$taskResult = Join-Path $taskComposeData 'result.txt'
$taskKinds = Join-Path $taskKindData 'kinds.txt'
$taskCompose = Join-Path $taskComposeData 'compose.txt'
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds(45)
  while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
    Start-Sleep -Milliseconds 250
  }
  if(!(Test-Path -LiteralPath $taskResult)) {
    throw "Compose probe did not finish; inspect $taskRoot\tc-modloader-data\loader.log"
  }
  $taskText = [IO.File]::ReadAllText($taskResult)
  $taskText
  if(!$taskText.StartsWith('PASS ')) { throw "Compose probe failed: $taskText" }
  if(!(Test-Path -LiteralPath $taskKinds)) {
    throw "The pin probe wrote no dump this session: $taskKinds"
  }
  if(!(Test-Path -LiteralPath $taskCompose)) {
    throw "The compose probe wrote no dump this session: $taskCompose"
  }
  $taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
  $taskLines = Get-Content -LiteralPath $taskLog
  foreach($taskExpected in @(
      'Component compose: built-in catalogue \d+ kinds, \d+ with no declared input pin, \d+ with a shape',
      'compose count=\d+',
      'compose without-declared-inputs=\d+ shaped=\d+ nameless=[1-9]\d*',
      # A named digital part, read through the service: same pins the pin probe
      # prints for kind 0x04 (in (-1,-1) / (-1,1), out (2,0), 1 bit each).
      'compose find kind=0x04 status=0 name="AND" in=2 out=1 in0=\(-1,-1,w1\) in1=\(-1,1,w1\) out0=\(2,0,w1\)',
      # The stable spellings, including the two names this build repeats and the
      # case-folded one.
      'compose spelling "builtin/AND" status=0 kind=0x04 name="AND"',
      'compose spelling "builtin/and" status=0 kind=0x04 name="AND"',
      'compose spelling "builtin/Delay Line" status=0 kind=0x0d name="Delay Line"',
      'compose spelling "builtin/Delay Line#2" status=0 kind=0x37 name="Delay Line"',
      # Names this build repeats: the ranks follow ascending kind order.  These
      # lines pin this build's facts (the loader log prints them from the
      # catalogue, and the probe checks the rule itself for every group).
      'compose duplicates name="AND" kinds=0x04,0x14 ids=builtin/AND,builtin/AND#2',
      'compose duplicates name="Delay Line" kinds=0x0d,0x37 ids=builtin/Delay Line,builtin/Delay Line#2',
      'compose duplicates name="Splitter" kinds=0x11,0x2f,0x63,0x64,0x6d,0x6e ids=builtin/Splitter,builtin/Splitter#2,builtin/Splitter#3,builtin/Splitter#4,builtin/Splitter#5,builtin/Splitter#6',
      # A slot the game keeps without a name: spelled by kind, no invented name.
      'compose nameless status=0 kind=0x7b name="" in=0 out=0',
      'compose refusals unknown-kind=-4 unknown-id=-4 pin-range=-3 bad-direction=-2 short-buffer=-2')){
    if(!($taskLines | Where-Object { $_ -match $taskExpected })){
      $taskLines | Where-Object { $_ -match 'compose' } | Select-Object -Last 10
      throw "Missing compose evidence: $taskExpected"
    }
  }

  # The two readers of the prototype table have to agree, entry for entry.
  $taskParse = {
    param([string]$Path)
    $map = @{}
    foreach($line in Get-Content -LiteralPath $Path) {
      # The pin probe prints a kind the way Nim does (0x4 for 0x04), so the key is
      # normalised by value instead of by spelling.
      if($line -notmatch '^(0x[0-9a-f]{1,2}) name="([^"]*)"') { continue }
      $kind = '0x{0:x2}' -f [Convert]::ToInt32($Matches[1].Substring(2), 16)
      $entry = [pscustomobject]@{
        Kind = $kind; Name = $Matches[2]; Inputs = 0; Outputs = 0; Pins = @{}
      }
      if($line -match 'inputs=(\d+) outputs=(\d+)') {
        $entry.Inputs = [int]$Matches[1]; $entry.Outputs = [int]$Matches[2]
      }
      foreach($pin in [regex]::Matches($line, '(in|out)(\d+)=\((-?\d+),(-?\d+),w(\d+)\)')) {
        $entry.Pins[$pin.Groups[1].Value + $pin.Groups[2].Value] = @(
          [int]$pin.Groups[3].Value, [int]$pin.Groups[4].Value, $pin.Groups[5].Value)
      }
      $map[$kind] = $entry
    }
    return $map
  }
  $taskKindMap = & $taskParse $taskKinds
  $taskComposeMap = & $taskParse $taskCompose
  if($taskKindMap.Count -ne $taskComposeMap.Count) {
    throw "The two readers disagree on how many kinds exist: pin probe $($taskKindMap.Count), compose $($taskComposeMap.Count)"
  }
  foreach($taskKind in $taskKindMap.Keys) {
    if(!$taskComposeMap.ContainsKey($taskKind)) {
      throw "The catalogue is missing kind $taskKind (the pin probe reports it)"
    }
    $taskLeft = $taskKindMap[$taskKind]; $taskRight = $taskComposeMap[$taskKind]
    if($taskLeft.Name -ne $taskRight.Name) {
      throw "Kind $taskKind name mismatch: pin probe '$($taskLeft.Name)', compose '$($taskRight.Name)'"
    }
    if($taskLeft.Inputs -ne $taskRight.Inputs -or $taskLeft.Outputs -ne $taskRight.Outputs) {
      throw "Kind $taskKind pin counts mismatch: pin probe $($taskLeft.Inputs)/$($taskLeft.Outputs), compose $($taskRight.Inputs)/$($taskRight.Outputs)"
    }
    foreach($taskPin in $taskLeft.Pins.Keys) {
      if(!$taskRight.Pins.ContainsKey($taskPin)) {
        throw "Kind $taskKind pin $taskPin missing from the catalogue"
      }
      $taskA = $taskLeft.Pins[$taskPin]; $taskB = $taskRight.Pins[$taskPin]
      if($taskA[0] -ne $taskB[0] -or $taskA[1] -ne $taskB[1] -or $taskA[2] -ne $taskB[2]) {
        throw "Kind $taskKind pin $taskPin mismatch: pin probe ($($taskA[0]),$($taskA[1]),w$($taskA[2])), compose ($($taskB[0]),$($taskB[1]),w$($taskB[2]))"
      }
    }
  }
  "The catalogue and the pin probe agree on $($taskComposeMap.Count) built-in kinds"
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  if($taskProcess -and !$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
  }
  "Evidence: $taskTest"
}
