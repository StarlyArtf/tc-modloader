$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskFixture = Join-Path $taskRepo 'build\and2_component.data'
if (!(Test-Path -LiteralPath $taskFixture)) {
  throw "Missing fixture; run build.ps1 first: $taskFixture"
}
$taskTest = Join-Path $taskRepo ('build\component-placement-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\test.component-placement'
$taskPackage = Join-Path $taskTest 'package'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods'),(Join-Path $taskData 'fixtures'),(Join-Path $taskPackage 'native') | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath $taskFixture -Destination (Join-Path $taskData 'fixtures\and2_component.data')
Copy-Item -LiteralPath (Join-Path $taskRepo 'build\component-placement-probe.dll') -Destination (Join-Path $taskPackage 'native\probe.dll')
'{"format":2,"id":"test.component-placement","name":"Development component placement probe","version":"0.0.1","native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding utf8
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRoot 'mods\probe.mod') | Out-Null
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.component-placement
if($LASTEXITCODE){throw 'Component placement probe package apply failed'}
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskProcess = $null
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = [DateTime]::UtcNow.AddSeconds(45)
  $taskResult = Join-Path $taskData 'result.txt'
  while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
    Start-Sleep -Milliseconds 250
  }
  if(!(Test-Path -LiteralPath $taskResult)) {
    throw "Component placement probe did not finish; inspect $taskRoot\tc-modloader-data\loader.log"
  }
  $taskText = [IO.File]::ReadAllText($taskResult)
  $taskText
  if(!$taskText.StartsWith('PASS ')) { throw "Component placement probe failed: $taskText" }
  # Custom-prototype pin path (Board V3+V4+V5).  The placed instance is the only
  # real-machine custom component this repository produces: its definition has
  # two 1-bit input pins and one 1-bit output pin, and the game normalises their
  # offsets into the prototype the V5 read reports.
  $taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
  if(!(Test-Path -LiteralPath $taskLog)) { throw "Loader log missing: $taskLog" }
  $taskLines = Get-Content -LiteralPath $taskLog
  foreach($taskExpected in @(
      'component placement objects status=0 components=2',
      'component placement custom pins status=-7 inputs=2 outputs=1 kind=0x4e',
      'component placement pins p0=i\(-1,-1,w1\) p1=i\(-1,0,w1\) p2=o\(2,-1,w1\) written=3',
      # The verified delete entry turns the appended record into the same kind-0
      # tombstone undo uses: sequence length stays 2, live count falls by one,
      # and the public V3/V4 lookup no longer finds the coordinate.
      'component deletion candidate before=2 after=2 live=1->0 found=0',
      # Board V edits through the command bus: submit, execute, and the board
      # gains exactly the component the command asked for by reusing that slot.
      'component placement command submit=0 request=\d+ before=2',
      'component placement command complete state=3 result=0 submitted=\d+ completed=\d+',
      'component placement command board before=2 after=2 live=0->1 found=1',
      # Two edits staged in one V2 transaction: committed, both steps, and the
      # board gains both components.
      'component placement transaction submit=0 request=\d+ before=2',
      'component placement transaction complete state=4 result=0 staged=2 completed=2',
      'component placement transaction board before=2 after=4 found=2',
      # Undo and redo: one undo reverts exactly one staged step (the other stays),
      # the removed slot is left as a zero-filled tombstone, and redo puts it back.
      'component placement undo state=3 result=0',
      'component placement undo points=30,0:0 20,6:1 24,10:1 28,10:0',
      'component placement undo components \[0x00\(0,0\)\] \[0x4e\(20,6\)c\] \[0x4e\(24,10\)c\] \[0x00\(0,0\)\]',
      'component placement redo state=3 result=0 components=4 .*restored=1')){
    if(!($taskLines | Where-Object { $_ -match $taskExpected })){
      $taskLines | Where-Object { $_ -match 'component placement' } | Select-Object -Last 8
      throw "Missing custom-prototype pin evidence: $taskExpected"
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
