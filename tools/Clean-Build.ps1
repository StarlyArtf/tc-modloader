# Removes playtest sandboxes (full game copies, ~300 MB each) and other
# regenerable debris from build/ while keeping the small build outputs,
# generated fixtures and probe DLLs.  Run from the source directory:
#   powershell -ExecutionPolicy Bypass -File tools\Clean-Build.ps1
$ErrorActionPreference = 'Stop'
$taskBuild = Join-Path (Split-Path $PSScriptRoot) 'build'
if(!(Test-Path -LiteralPath $taskBuild)) { 'No build directory'; exit 0 }
$taskPatterns = @(
  'and-component-playtest-*',
  'components-playtest-*',
  'component-placement-playtest-*',
  'component-persistence-playtest-*',
  'component-cost-playtest-*',
  'e2e-*',
  'probe-*',
  'repro-*',
  'custom-or-playtest-*',
  'cost-watch-smoke-*',
  'circuit-and-playtest-*',
  'installer-test-*',
  'installer-verify*',
  'release-*',
  'native-test-*',
  'kind-list-playtest-*',
  'sim-state-playtest-*',
  'byte-adder-smoke-*',
  '*-smoke-*',
  'timing-*',
  'tmp',
  'tmp-*',
  # The playtest scripts that keep their own full game copy name it either
  # *-sandbox or ui-playtest-<guid>; leaving those out is what let a checkout
  # reach 50 GB of sandboxes.
  '*-sandbox',
  'ui-playtest-*',
  'hook-chain-test-*',
  'hc-test*',
  # Per-run output directories of the probe playtests.
  'pin-label-out',
  'palette-check',
  'manual-check',
  '*-out',
  '*-check'
)
$taskFilePatterns = @('*.keep','*.bak','*.old','*.old.mod','native-logic-source*.txt')
$taskRemoved = @()
$taskFreed = 0
function Remove-Target([string]$path) {
  # Files copied out of the game are often read-only, and some locked-down
  # shells reject -Force, so try it and fall back to a plain recursive delete.
  try { Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop }
  catch { Remove-Item -LiteralPath $path -Recurse -ErrorAction SilentlyContinue }
  return -not (Test-Path -LiteralPath $path)
}
foreach($taskPattern in $taskPatterns) {
  foreach($taskDir in Get-ChildItem -LiteralPath $taskBuild -Directory -Filter $taskPattern) {
    $taskFull = $taskDir.FullName
    # Only ever delete inside this repository's build directory.
    if(!$taskFull.StartsWith($taskBuild,[StringComparison]::OrdinalIgnoreCase)) {
      throw "Refusing to delete outside build: $taskFull"
    }
    $taskBytes = (Get-ChildItem -LiteralPath $taskFull -Recurse -File -ErrorAction SilentlyContinue |
      Measure-Object -Property Length -Sum).Sum
    if(Remove-Target $taskFull) {
      $taskRemoved += $taskDir.Name
      if($taskBytes) { $taskFreed += $taskBytes }
    } else { Write-Warning "Could not remove $taskFull" }
  }
}
foreach($taskPattern in $taskFilePatterns) {
  foreach($taskFile in Get-ChildItem -LiteralPath $taskBuild -File -Filter $taskPattern) {
    $taskFull = $taskFile.FullName
    if(!$taskFull.StartsWith($taskBuild,[StringComparison]::OrdinalIgnoreCase)) {
      throw "Refusing to delete outside build: $taskFull"
    }
    if(Remove-Target $taskFull) {
      $taskRemoved += $taskFile.Name
      $taskFreed += $taskFile.Length
    } else { Write-Warning "Could not remove $taskFull" }
  }
}
if($taskRemoved.Count) {
  ('Removed {0} item(s), freed {1:N1} MB:' -f $taskRemoved.Count,($taskFreed / 1MB))
  $taskRemoved
} else { 'Nothing to remove' }
$taskSize = (Get-ChildItem -LiteralPath $taskBuild -Recurse -File |
  Measure-Object -Property Length -Sum).Sum / 1MB
('build/ size: {0:N1} MB' -f $taskSize)
