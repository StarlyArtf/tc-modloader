param(
  [switch]$AllowDirty,
  [switch]$Force,
  [string]$OutputRoot = ''
)
$ErrorActionPreference = 'Stop'
$taskArguments = @{
  UseExistingBuild = $true
  SkipTests = $true
  AllowDirty = [bool]$AllowDirty
  Force = [bool]$Force
}
if ($OutputRoot) { $taskArguments.OutputRoot = $OutputRoot }
& (Join-Path $PSScriptRoot 'tools\release.ps1') @taskArguments
if ($LASTEXITCODE) { exit $LASTEXITCODE }

