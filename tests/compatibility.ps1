$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
& (Join-Path $taskRepo 'tools\generate-compat.ps1') -Check
if ($LASTEXITCODE) { throw 'Generated compatibility header is stale' }

$taskManifest = Get-Content -LiteralPath (Join-Path $taskRepo 'compat\profiles.json') -Raw | ConvertFrom-Json
$taskProfiles = @($taskManifest.profiles)
$taskIds = @($taskProfiles | ForEach-Object { [string]$_.id })
if (($taskIds | Sort-Object -Unique).Count -ne $taskIds.Count) { throw 'Duplicate compatibility profile id' }
if (@($taskProfiles | Where-Object { $_.current }).Count -ne 1) { throw 'Expected exactly one current compatibility profile' }
foreach ($taskProfile in $taskProfiles) {
  if ([string]$taskProfile.status -notin @('supported', 'diagnostic', 'blocked')) { throw "Invalid profile status: $($taskProfile.status)" }
  if ([string]$taskProfile.files.executable.sha256 -notmatch '^[0-9a-f]{64}$') { throw "Invalid executable hash: $($taskProfile.id)" }
  if ([string]$taskProfile.files.engine.sha256 -notmatch '^[0-9a-f]{64}$') { throw "Invalid engine hash: $($taskProfile.id)" }
}
"PASS compatibility contract: $($taskProfiles.Count) profile(s), current=$((@($taskProfiles | Where-Object { $_.current }))[0].id)"
