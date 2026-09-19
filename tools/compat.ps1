param(
  [string]$GameDirectory = '',
  [string]$ManifestPath = '',
  [switch]$Json
)

$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
if (!$GameDirectory) { $GameDirectory = Split-Path $taskRepo }
if (!$ManifestPath) { $ManifestPath = Join-Path $taskRepo 'compat\profiles.json' }
$taskRoot = [IO.Path]::GetFullPath($GameDirectory)
$taskManifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
if ([int]$taskManifest.schemaVersion -ne 1) { throw "Unsupported compatibility profile schema: $($taskManifest.schemaVersion)" }

function Get-FileEvidence([string]$Path) {
  if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
    return [ordered]@{ path = $Path; exists = $false; size = $null; sha256 = $null }
  }
  $taskFile = Get-Item -LiteralPath $Path
  return [ordered]@{
    path = $taskFile.FullName
    exists = $true
    size = [long]$taskFile.Length
    sha256 = (Get-FileHash -LiteralPath $taskFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
  }
}

$taskExecutableName = [string]$taskManifest.profiles[0].files.executable.name
$taskInstalledEngineName = [string]$taskManifest.profiles[0].files.engine.installedName
$taskBackupEngineName = [string]$taskManifest.profiles[0].files.engine.backupName
$taskExecutable = Get-FileEvidence (Join-Path $taskRoot $taskExecutableName)
$taskBackupPath = Join-Path $taskRoot $taskBackupEngineName
$taskEngineSource = if (Test-Path -LiteralPath $taskBackupPath -PathType Leaf) { 'backup' } else { 'installed' }
$taskEnginePath = if ($taskEngineSource -eq 'backup') { $taskBackupPath } else { Join-Path $taskRoot $taskInstalledEngineName }
$taskEngine = Get-FileEvidence $taskEnginePath

$taskMatches = @()
foreach ($taskProfile in $taskManifest.profiles) {
  $taskExeMatches = $taskExecutable.exists -and
    $taskExecutable.size -eq [long]$taskProfile.files.executable.size -and
    $taskExecutable.sha256 -ceq [string]$taskProfile.files.executable.sha256
  $taskEngineMatches = $taskEngine.exists -and
    $taskEngine.size -eq [long]$taskProfile.files.engine.size -and
    $taskEngine.sha256 -ceq [string]$taskProfile.files.engine.sha256
  if ($taskExeMatches -and $taskEngineMatches) { $taskMatches += $taskProfile }
}

$taskSupported = $taskMatches.Count -eq 1 -and [string]$taskMatches[0].status -eq 'supported'
$taskResult = [ordered]@{
  schemaVersion = 1
  supported = $taskSupported
  profileId = if ($taskMatches.Count -eq 1) { [string]$taskMatches[0].id } else { $null }
  gameVersion = if ($taskMatches.Count -eq 1) { [string]$taskMatches[0].gameVersion } else { $null }
  status = if ($taskMatches.Count -eq 1) { [string]$taskMatches[0].status } else { 'unknown' }
  engineSource = $taskEngineSource
  executable = $taskExecutable
  engine = $taskEngine
}

if ($Json) {
  $taskResult | ConvertTo-Json -Depth 5
} elseif ($taskSupported) {
  "SUPPORTED $($taskResult.profileId): Turing Complete $($taskResult.gameVersion), engine=$taskEngineSource"
  "  exe    $($taskExecutable.sha256)  $($taskExecutable.path)"
  "  engine $($taskEngine.sha256)  $($taskEngine.path)"
} else {
  "UNSUPPORTED: no compatibility profile matches $taskRoot"
  "  exe    $(if ($taskExecutable.sha256) { $taskExecutable.sha256 } else { 'missing' })  $($taskExecutable.path)"
  "  engine $(if ($taskEngine.sha256) { $taskEngine.sha256 } else { 'missing' })  $($taskEngine.path)"
}
if (!$taskSupported) { exit 2 }
