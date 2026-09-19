# Builds (or reuses) an isolated game copy for the native UI page playtests.
# The game, its assets and the user's saves are never touched: everything runs
# under build/ui-page-sandbox with its own USERPROFILE/APPDATA.
param(
  [string]$Sandbox = (Join-Path (Split-Path $PSScriptRoot) 'build\ui-page-sandbox'),
  [string]$Game = (Split-Path (Split-Path $PSScriptRoot)),
  [switch]$Recreate,
  [string[]]$Mods = @('dev.menu-demo','dev.menu-demo-peer')
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
Import-Module (Join-Path $PSScriptRoot 'lib\Sandbox.psm1') -Force
$taskSandbox = New-TcGameSandbox -Repository $taskRepo -Sandbox $Sandbox -Game $Game -Mods $Mods -Recreate:$Recreate
"Sandbox ready: $($taskSandbox.Sandbox)"
