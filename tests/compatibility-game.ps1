$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskJson = & (Join-Path $taskRepo 'tools\compat.ps1') -GameDirectory $taskGame -Json
if ($LASTEXITCODE) { throw 'Pinned game did not match a supported compatibility profile' }
$taskResult = $taskJson | ConvertFrom-Json
if (!$taskResult.supported -or $taskResult.profileId -ne 'tc-win64-2.1.334') { throw 'Unexpected compatibility identification result' }
"PASS pinned game compatibility: $($taskResult.profileId), engine source=$($taskResult.engineSource)"
