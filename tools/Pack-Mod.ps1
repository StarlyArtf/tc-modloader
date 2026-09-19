param([Parameter(Mandatory=$true)][string]$Source,[Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
$taskSource=(Resolve-Path -LiteralPath $Source).Path
$taskOutput=[IO.Path]::GetFullPath($Output)
if (!(Test-Path -LiteralPath (Join-Path $taskSource 'mod.json'))) { throw 'mod.json must be at the source folder root.' }
if ($taskOutput.StartsWith($taskSource+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Output must be outside the source folder.' }
if (Test-Path -LiteralPath $taskOutput) { throw 'Output already exists. Use another filename.' }
Import-Module (Join-Path $PSScriptRoot 'ReleaseTools.psm1') -Force
New-TcDeterministicZip -SourceDirectory $taskSource -Output $taskOutput | Out-Null
Read-TcModArchive -Package $taskOutput -RequireDeterministic | Out-Null
Write-Output $taskOutput
