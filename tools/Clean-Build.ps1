# Removes playtest sandboxes (full game copies, ~300 MB each) and other
# regenerable debris from build/ while keeping the small build outputs,
# generated fixtures and probe DLLs.  Run from the source directory:
#   powershell -ExecutionPolicy Bypass -File tools\Clean-Build.ps1
#
# Every *directory* under build/ is debris: the per-run sandboxes are copies of
# the game, the *-package directories are staging areas build.ps1 recreates from
# scratch, and the *-out directories are probe output.  Sweeping the directory
# instead of a pattern list is what keeps a new playtest script from quietly
# leaving its sandbox behind - the old pattern list missed the punch-tape,
# word-watchee, save-test, test-<guid> and gate-delay-* sandboxes, and a working
# copy grew to 140 GB.  Only files are matched by pattern, because a few of them
# (the generated board fixtures, the probe DLLs) are worth keeping.
$ErrorActionPreference = 'Stop'
$taskBuild = Join-Path (Split-Path $PSScriptRoot) 'build'
if(!(Test-Path -LiteralPath $taskBuild)) { 'No build directory'; exit 0 }
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
foreach($taskDir in Get-ChildItem -LiteralPath $taskBuild -Directory) {
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
