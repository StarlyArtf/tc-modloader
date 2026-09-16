$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskLevel = if($env:TC_LEVEL) { $env:TC_LEVEL } else { 'and_gate' }
if($taskLevel -notin @('and_gate','sandbox','foundry')) { throw 'Unsupported timing test level' }
$taskFixture = if($env:TC_FIXTURE) { $env:TC_FIXTURE } else { Join-Path $taskRepo 'build\and2_component.data' }
$taskSolution = if($env:TC_SOLUTION) { $env:TC_SOLUTION } else { Join-Path $taskRepo 'build\and2_solution.data' }
foreach($taskFile in @($taskFixture,$taskSolution)) {
  if(!(Test-Path -LiteralPath $taskFile)) { throw "Missing $taskFile; run build.ps1 first" }
}

# Modes: plain (no score table change) and insert (add_cost(0x4e, ...)).
$taskModes = if($env:TC_COST_MODES) { $env:TC_COST_MODES -split ',' } else { @('builtin','plain','insert') }
$taskEvidence = @()

foreach($taskMode in $taskModes) {
  $taskTest = Join-Path $taskRepo ('build\component-cost-playtest-' + $taskMode + '-' + [guid]::NewGuid().ToString('N'))
  $taskRoot = Join-Path $taskTest 'game'
  $taskProfile = Join-Path $taskTest 'home'
  $taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\test.component-cost'
  $taskPackage = Join-Path $taskTest 'package'
  $taskCategory = if($taskLevel -eq 'sandbox') { 'architecture' } else { $taskLevel }
  $taskSchema = Join-Path $taskProfile "AppData\Roaming\Turing Complete Mods\profiles\default\schematics\$taskCategory\Default"
  New-Item -ItemType Directory -Force $taskRoot,$taskProfile,$taskSchema,(Join-Path $taskRoot 'mods'),(Join-Path $taskData 'fixtures'),(Join-Path $taskPackage 'native') | Out-Null
  foreach($taskDir in @('asset','campaign','translations')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
  }
  foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
  }
  Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
  Copy-Item -LiteralPath $taskFixture -Destination (Join-Path $taskData 'fixtures\and2_component.data')
  $taskInner = Join-Path (Split-Path $taskFixture) 'inner_component.data'
  if(Test-Path -LiteralPath $taskInner) { Copy-Item -LiteralPath $taskInner -Destination (Join-Path $taskData 'fixtures\inner_component.data') }
  Copy-Item -LiteralPath (Join-Path $taskRepo 'build\component-cost-probe.dll') -Destination (Join-Path $taskPackage 'native\probe.dll')
  '{"format":2,"id":"test.component-cost","name":"Development component cost probe","version":"0.0.1","native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding utf8
  & (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRoot 'mods\probe.mod') | Out-Null
  & (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.component-cost
  if($LASTEXITCODE){throw 'Component cost probe package apply failed'}
  Set-Content -LiteralPath (Join-Path $taskData 'mode.txt') -Value $taskMode -Encoding ascii
  Set-Content -LiteralPath (Join-Path $taskData 'level.txt') -Value $taskLevel -Encoding ascii

  $taskCircuitSource = if($taskMode -eq 'builtin') { Join-Path $taskRepo 'build\and2_solution_builtin.data' } else { $taskSolution }
  Copy-Item -LiteralPath $taskCircuitSource -Destination (Join-Path $taskSchema 'circuit.data') -Force
  if($taskLevel -ne 'and_gate') {
    $taskLevels=Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default\levels.txt'
    [IO.File]::WriteAllText($taskLevels,('"'+$taskLevel+'",false,"Default",'+"`n"))
  }
  $taskPreviousProfile = $env:USERPROFILE
  $taskPreviousAppData = $env:APPDATA
  try {
    $env:USERPROFILE = $taskProfile
    $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
    $taskResult = Join-Path $taskData 'result.txt'
    if(Test-Path -LiteralPath $taskResult) { Remove-Item -LiteralPath $taskResult -Force }
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline = [DateTime]::UtcNow.AddSeconds(60)
    while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
      Start-Sleep -Milliseconds 250
    }
    if(!(Test-Path -LiteralPath $taskResult)) {
      if(!$taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }
      throw "Cost probe ($taskMode) did not finish; inspect $taskRoot\tc-modloader-data\loader.log"
    }
    if(!$taskProcess.HasExited) {
      [void]$taskProcess.CloseMainWindow()
      if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
    }
    $taskText = [IO.File]::ReadAllText($taskResult)
    if($env:TC_EXPECT_DELAY -and $taskText -notmatch "(?m)^compiled delay=$([regex]::Escape($env:TC_EXPECT_DELAY))\r?$") {
      throw "Compiled delay assertion failed: expected $env:TC_EXPECT_DELAY; $taskText"
    }
    if($env:TC_EXPECT_GATES -and $taskText -notmatch "(?m)^compiled gates=$([regex]::Escape($env:TC_EXPECT_GATES))\r?$") {
      throw "Gate count assertion failed: expected $env:TC_EXPECT_GATES; $taskText"
    }
    if($env:TC_EXPECT_UI -eq '1' -and $env:TC_EXPECT_GATES -and $env:TC_EXPECT_DELAY -and
       $taskText -notmatch "build_scores calls=\d+ gates=$env:TC_EXPECT_GATES delay=$env:TC_EXPECT_DELAY flag=0") {
      throw "Displayed score assertion failed; $taskText"
    }
    if($env:TC_EXPECT_LOG) {
      $taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
      if(!(Select-String -Path $taskLog -Pattern $env:TC_EXPECT_LOG -Quiet)) {
        throw "Loader log assertion failed: expected /$env:TC_EXPECT_LOG/ in $taskLog"
      }
    }
    "==== mode: $taskMode ===="
    $taskText
    $taskEvidence += ("[" + $taskMode + "]`n" + $taskText)
    # Sandboxes are ~290 MB each; keep the report, drop the copy.
    $taskResolved = [IO.Path]::GetFullPath($taskTest)
    $taskBuildRoot = [IO.Path]::GetFullPath((Join-Path $taskRepo 'build')) + [IO.Path]::DirectorySeparatorChar
    if(!$taskResolved.StartsWith($taskBuildRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Sandbox cleanup escaped build directory' }
    Remove-Item -LiteralPath $taskResolved -Recurse -Force
  } finally {
    $env:USERPROFILE = $taskPreviousProfile
    $env:APPDATA = $taskPreviousAppData
  }
}

$taskOut = if($env:TC_REPORT) { $env:TC_REPORT } else { Join-Path $taskRepo 'build\component-cost-report.txt' }
Set-Content -LiteralPath $taskOut -Value ($taskEvidence -join "`n") -Encoding utf8
"Report: $taskOut"
