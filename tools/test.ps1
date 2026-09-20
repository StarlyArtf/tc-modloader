param(
  [ValidateSet('fast', 'host', 'game', 'all')]
  [string]$Tier = 'fast',
  [string[]]$Name = @(),
  [switch]$List,
  [switch]$NoBuild,
  [switch]$KeepGoing,
  [string]$ResultsDirectory = ''
)

$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskCatalogPath = Join-Path $taskRepo 'tests\test-catalog.json'
$taskCatalog = Get-Content -LiteralPath $taskCatalogPath -Raw | ConvertFrom-Json

if ($taskCatalog.schemaVersion -ne 1) {
  throw "Unsupported test catalog schema: $($taskCatalog.schemaVersion)"
}

$taskKnownRunners = @('powershell', 'node')
$taskIds = @{}
foreach ($taskTest in $taskCatalog.tests) {
  if (!$taskTest.id -or $taskIds.ContainsKey([string]$taskTest.id)) {
    throw "Duplicate or empty test id in $taskCatalogPath`: $($taskTest.id)"
  }
  $taskIds[[string]$taskTest.id] = $true
  if ($taskKnownRunners -notcontains [string]$taskTest.runner) {
    throw "Unknown runner '$($taskTest.runner)' for $($taskTest.id)"
  }
  if (!(Test-Path -LiteralPath (Join-Path $taskRepo $taskTest.path) -PathType Leaf)) {
    throw "Missing test path for $($taskTest.id): $($taskTest.path)"
  }
  if ([int]$taskTest.timeoutSeconds -lt 1) {
    throw "Invalid timeout for $($taskTest.id)"
  }
}

function Test-Property([object]$Object, [string]$Name) {
  return $null -ne $Object.PSObject.Properties[$Name]
}

function Quote-ProcessArgument([string]$Value) {
  if ($null -eq $Value -or $Value.Length -eq 0) { return '""' }
  if ($Value -notmatch '[\s"]') { return $Value }
  $taskOut = '"'
  $taskBackslashes = 0
  foreach ($taskChar in $Value.ToCharArray()) {
    if ($taskChar -eq '\') {
      $taskBackslashes++
      continue
    }
    if ($taskChar -eq '"') {
      $taskOut += (('\' * ($taskBackslashes * 2 + 1)) -join '') + '"'
      $taskBackslashes = 0
      continue
    }
    if ($taskBackslashes) {
      $taskOut += (('\' * $taskBackslashes) -join '')
      $taskBackslashes = 0
    }
    $taskOut += $taskChar
  }
  if ($taskBackslashes) { $taskOut += (('\' * ($taskBackslashes * 2)) -join '') }
  return $taskOut + '"'
}

function Resolve-TestCommand([object]$Test) {
  $taskPath = Join-Path $taskRepo $Test.path
  $taskArguments = @()
  if ([string]$Test.runner -eq 'powershell') {
    $taskExecutable = (Get-Command powershell.exe -ErrorAction Stop).Source
    $taskArguments += @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $taskPath)
  } else {
    $taskExecutable = (Get-Command node.exe -ErrorAction Stop).Source
    $taskArguments += $taskPath
  }
  if (Test-Property $Test 'arguments') {
    foreach ($taskArgument in $Test.arguments) { $taskArguments += [string]$taskArgument }
  }
  return [pscustomobject]@{ Executable = $taskExecutable; Arguments = $taskArguments }
}

function Invoke-CatalogTest([object]$Test, [string]$OutputPath, [string]$ErrorPath) {
  $taskCommand = Resolve-TestCommand $Test
  $taskInfo = New-Object Diagnostics.ProcessStartInfo
  $taskInfo.FileName = $taskCommand.Executable
  $taskInfo.Arguments = (($taskCommand.Arguments | ForEach-Object { Quote-ProcessArgument $_ }) -join ' ')
  $taskInfo.WorkingDirectory = $taskRepo
  $taskInfo.UseShellExecute = $false
  $taskInfo.CreateNoWindow = $true
  $taskInfo.RedirectStandardOutput = $true
  $taskInfo.RedirectStandardError = $true
  $taskInfo.EnvironmentVariables['TC_TEST_RUNNER'] = '1'

  $taskProcess = New-Object Diagnostics.Process
  $taskProcess.StartInfo = $taskInfo
  $taskStarted = [DateTime]::UtcNow
  [void]$taskProcess.Start()
  $taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
  $taskStderr = $taskProcess.StandardError.ReadToEndAsync()
  $taskTimedOut = !$taskProcess.WaitForExit([int]$Test.timeoutSeconds * 1000)
  if ($taskTimedOut) {
    try { $taskProcess.Kill() } catch {}
    [void]$taskProcess.WaitForExit(5000)
  }
  $taskStdout.Wait()
  $taskStderr.Wait()
  [IO.File]::WriteAllText($OutputPath, $taskStdout.Result, [Text.UTF8Encoding]::new($false))
  [IO.File]::WriteAllText($ErrorPath, $taskStderr.Result, [Text.UTF8Encoding]::new($false))
  $taskExitCode = if ($taskTimedOut) { -1 } else { $taskProcess.ExitCode }
  return [pscustomobject]@{
    ExitCode = $taskExitCode
    TimedOut = $taskTimedOut
    DurationSeconds = [Math]::Round(([DateTime]::UtcNow - $taskStarted).TotalSeconds, 3)
    Stdout = $taskStdout.Result
    Stderr = $taskStderr.Result
  }
}

function Test-MatchesName([object]$Test) {
  if (!$Name.Count) { return $true }
  foreach ($taskPattern in $Name) {
    if ([string]$Test.id -like $taskPattern) { return $true }
  }
  return $false
}

if ($List) {
  $taskCatalog.tests | ForEach-Object {
    [pscustomobject]@{
      Id = $_.id
      Tier = $_.tier
      Game = [bool]$_.requiresGame
      Manual = if (Test-Property $_ 'manual') { [bool]$_.manual } else { $false }
      Timeout = $_.timeoutSeconds
      Description = $_.description
    }
  } | Format-Table -AutoSize
  exit 0
}

$taskSelected = @($taskCatalog.tests | Where-Object {
  $taskManual = (Test-Property $_ 'manual') -and [bool]$_.manual
  if ($Name.Count) {
    Test-MatchesName $_
  } else {
    !$taskManual -and ($Tier -eq 'all' -or [string]$_.tier -eq $Tier)
  }
})

if (!$taskSelected.Count) {
  throw 'No tests matched. Use -List to inspect the catalog.'
}

$taskNeedsBuild = @($taskSelected | Where-Object { [bool]$_.requiresBuild }).Count -gt 0
$taskHasBuild = @($taskSelected | Where-Object { $_.id -eq 'build' }).Count -gt 0
if (!$NoBuild -and $taskNeedsBuild -and !$taskHasBuild) {
  $taskBuild = $taskCatalog.tests | Where-Object { $_.id -eq 'build' } | Select-Object -First 1
  $taskSelected = @($taskBuild) + $taskSelected
}

$taskNeedsGame = @($taskSelected | Where-Object { [bool]$_.requiresGame }).Count -gt 0
if ($taskNeedsGame) {
  $taskGame = Split-Path $taskRepo
  foreach ($taskFile in @('Turing Complete.exe', 'compile.dll', 'tc_game_engine.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $taskGame $taskFile) -PathType Leaf)) {
      throw "The selected tests require the pinned game beside the repository; missing '$taskFile' in $taskGame"
    }
  }
}

if (!$ResultsDirectory) { $ResultsDirectory = Join-Path $taskRepo 'build\test-results' }
$taskResultsDirectory = [IO.Path]::GetFullPath($ResultsDirectory)
New-Item -ItemType Directory -Force $taskResultsDirectory | Out-Null

Write-Host ("Running {0} test(s): {1}" -f $taskSelected.Count, (($taskSelected | ForEach-Object { $_.id }) -join ', '))
$taskResults = @()
$taskSuiteStarted = [DateTime]::UtcNow
foreach ($taskTest in $taskSelected) {
  $taskOutputPath = Join-Path $taskResultsDirectory ($taskTest.id + '.stdout.log')
  $taskErrorPath = Join-Path $taskResultsDirectory ($taskTest.id + '.stderr.log')
  Write-Host ("[{0}/{1}] {2}" -f ($taskResults.Count + 1), $taskSelected.Count, $taskTest.id)
  $taskResult = Invoke-CatalogTest $taskTest $taskOutputPath $taskErrorPath
  $taskPassed = !$taskResult.TimedOut -and $taskResult.ExitCode -eq 0
  $taskStatus = if ($taskPassed) { 'PASS' } elseif ($taskResult.TimedOut) { 'TIMEOUT' } else { 'FAIL' }
  Write-Host ("{0} {1} ({2:N1}s)" -f $taskStatus, $taskTest.id, $taskResult.DurationSeconds)
  if (!$taskPassed) {
    if ($taskResult.Stdout) { Write-Host $taskResult.Stdout.TrimEnd() }
    if ($taskResult.Stderr) { Write-Warning $taskResult.Stderr.TrimEnd() }
  }
  $taskResults += [pscustomobject]@{
    id = [string]$taskTest.id
    tier = [string]$taskTest.tier
    passed = $taskPassed
    timedOut = $taskResult.TimedOut
    exitCode = $taskResult.ExitCode
    durationSeconds = $taskResult.DurationSeconds
    stdout = $taskOutputPath
    stderr = $taskErrorPath
  }
  if (!$taskPassed -and !$KeepGoing) { break }
}

$taskFinished = [DateTime]::UtcNow
$taskFailed = @($taskResults | Where-Object { !$_.passed })
$taskSummary = [pscustomobject]@{
  schemaVersion = 1
  tier = $Tier
  startedUtc = $taskSuiteStarted.ToString('o')
  finishedUtc = $taskFinished.ToString('o')
  durationSeconds = [Math]::Round(($taskFinished - $taskSuiteStarted).TotalSeconds, 3)
  passed = $taskResults.Count - $taskFailed.Count
  failed = $taskFailed.Count
  tests = $taskResults
}
$taskSummary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskResultsDirectory 'results.json') -Encoding utf8

$taskXmlPath = Join-Path $taskResultsDirectory 'results.xml'
$taskSettings = New-Object Xml.XmlWriterSettings
$taskSettings.Indent = $true
$taskSettings.Encoding = New-Object Text.UTF8Encoding($false)
$taskXml = [Xml.XmlWriter]::Create($taskXmlPath, $taskSettings)
try {
  $taskXml.WriteStartDocument()
  $taskXml.WriteStartElement('testsuite')
  $taskXml.WriteAttributeString('name', 'tc-modloader')
  $taskXml.WriteAttributeString('tests', [string]$taskResults.Count)
  $taskXml.WriteAttributeString('failures', [string]$taskFailed.Count)
  $taskXml.WriteAttributeString('time', [string]$taskSummary.durationSeconds)
  foreach ($taskResult in $taskResults) {
    $taskXml.WriteStartElement('testcase')
    $taskXml.WriteAttributeString('name', $taskResult.id)
    $taskXml.WriteAttributeString('classname', ('tc-modloader.' + $taskResult.tier))
    $taskXml.WriteAttributeString('time', [string]$taskResult.durationSeconds)
    if (!$taskResult.passed) {
      $taskXml.WriteStartElement('failure')
      $taskXml.WriteAttributeString('message', $(if ($taskResult.timedOut) { 'timed out' } else { 'non-zero exit' }))
      $taskXml.WriteString("exit=$($taskResult.exitCode); stdout=$($taskResult.stdout); stderr=$($taskResult.stderr)")
      $taskXml.WriteEndElement()
    }
    $taskXml.WriteEndElement()
  }
  $taskXml.WriteEndElement()
  $taskXml.WriteEndDocument()
} finally {
  $taskXml.Dispose()
}

Write-Host ("Result: {0} passed, {1} failed; reports: {2}" -f $taskSummary.passed, $taskSummary.failed, $taskResultsDirectory)
if ($taskFailed.Count) { exit 1 }
