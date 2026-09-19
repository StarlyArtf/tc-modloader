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
# A test-catalog entry can only pass one token per argument, and `-Mods a b`
# would bind "b" to the next positional parameter (-Game), so a comma-separated
# list is accepted as well: -Mods dev.menu-demo,dev.menu-demo-peer.
$Mods = @($Mods | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
Import-Module (Join-Path $PSScriptRoot 'lib\Sandbox.psm1') -Force
$taskSandbox = New-TcGameSandbox -Repository $taskRepo -Sandbox $Sandbox -Game $Game -Mods $Mods -Recreate:$Recreate
"Sandbox ready: $($taskSandbox.Sandbox)"
