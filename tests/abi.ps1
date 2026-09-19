$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
& (Join-Path $taskRepo 'tools\abi.ps1')
if ($LASTEXITCODE) { throw 'SDK ABI snapshot comparison failed' }
