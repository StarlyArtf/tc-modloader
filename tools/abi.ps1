param(
  [switch]$Update,
  [switch]$Json
)

$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskCompilerRoot = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskCompiler = Join-Path $taskCompilerRoot 'g++.exe'
$taskBuild = Join-Path $taskRepo 'build'
$taskExecutable = Join-Path $taskBuild 'abi-snapshot.exe'
$taskBaselinePath = Join-Path $taskRepo 'abi\windows-x64.json'
New-Item -ItemType Directory -Force $taskBuild | Out-Null

& $taskCompiler -std=c++17 -O2 -Wall -Wextra -pedantic -I$taskRepo `
  (Join-Path $taskRepo 'tools\abi-snapshot.cpp') -o $taskExecutable
if ($LASTEXITCODE) { throw 'ABI snapshot probe compilation failed' }
$taskLines = & $taskExecutable
if ($LASTEXITCODE) { throw 'ABI snapshot probe failed' }

$taskRecords = [ordered]@{}
foreach ($taskLine in $taskLines) {
  $taskParts = [string]$taskLine -split '=', 2
  if ($taskParts.Count -ne 2 -or !$taskParts[0] -or $taskRecords.Contains($taskParts[0])) { throw "Invalid ABI probe row: $taskLine" }
  $taskValue = 0L
  if (![long]::TryParse($taskParts[1], [Globalization.NumberStyles]::Integer, [Globalization.CultureInfo]::InvariantCulture, [ref]$taskValue)) {
    throw "Invalid ABI probe value: $taskLine"
  }
  $taskRecords[$taskParts[0]] = $taskValue
}
$taskSnapshot = [ordered]@{
  schemaVersion = 1
  target = 'windows-x64-mingw-ucrt'
  records = $taskRecords
}

if ($Json) {
  $taskSnapshot | ConvertTo-Json -Depth 5
  exit 0
}
if ($Update) {
  $taskParent = Split-Path $taskBaselinePath
  New-Item -ItemType Directory -Force $taskParent | Out-Null
  $taskText = ($taskSnapshot | ConvertTo-Json -Depth 5).Replace("`r`n", "`n") + "`n"
  [IO.File]::WriteAllText($taskBaselinePath, $taskText, [Text.UTF8Encoding]::new($false))
  "Updated ABI baseline: $taskBaselinePath ($($taskRecords.Count) records)"
  exit 0
}
if (!(Test-Path -LiteralPath $taskBaselinePath -PathType Leaf)) { throw 'ABI baseline is missing; review and run tools/abi.ps1 -Update' }
$taskBaseline = Get-Content -LiteralPath $taskBaselinePath -Raw | ConvertFrom-Json
if ([int]$taskBaseline.schemaVersion -ne 1 -or [string]$taskBaseline.target -ne [string]$taskSnapshot.target) {
  throw 'ABI baseline schema or target does not match this probe'
}
$taskExpected = @{}
foreach ($taskProperty in $taskBaseline.records.PSObject.Properties) { $taskExpected[$taskProperty.Name] = [long]$taskProperty.Value }
$taskDifferences = @()
foreach ($taskName in @($taskExpected.Keys + $taskRecords.Keys | Sort-Object -Unique)) {
  if (!$taskExpected.ContainsKey($taskName)) { $taskDifferences += "ADDED $taskName=$($taskRecords[$taskName])"; continue }
  if (!$taskRecords.Contains($taskName)) { $taskDifferences += "REMOVED $taskName (expected $($taskExpected[$taskName]))"; continue }
  if ([long]$taskExpected[$taskName] -ne [long]$taskRecords[$taskName]) {
    $taskDifferences += "CHANGED $taskName expected=$($taskExpected[$taskName]) actual=$($taskRecords[$taskName])"
  }
}
if ($taskDifferences.Count) {
  throw "SDK ABI snapshot changed. Review compatibility before updating abi/windows-x64.json:`n$($taskDifferences -join "`n")"
}
"PASS SDK ABI snapshot: $($taskRecords.Count) records match $taskBaselinePath"
