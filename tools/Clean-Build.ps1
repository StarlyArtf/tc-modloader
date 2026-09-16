# Removes playtest sandboxes (full game copies) from build/ while keeping the
# small build outputs and generated fixtures.  Run from the source directory:
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
  'cost-watch-smoke-*',
  'circuit-and-playtest-*',
  'installer-test-*',
  'installer-verify*',
  'release-*',
  'native-test-*'
)
$taskRemoved = @()
foreach($taskPattern in $taskPatterns) {
  foreach($taskDir in Get-ChildItem -LiteralPath $taskBuild -Directory -Filter $taskPattern) {
    $taskFull = $taskDir.FullName
    # Only ever delete inside this repository's build directory.
    if(!$taskFull.StartsWith($taskBuild,[StringComparison]::OrdinalIgnoreCase)) {
      throw "Refusing to delete outside build: $taskFull"
    }
    Remove-Item -LiteralPath $taskFull -Recurse -Force
    $taskRemoved += $taskDir.Name
  }
}
if($taskRemoved.Count) { "Removed $($taskRemoved.Count) sandbox(es):"; $taskRemoved }
else { 'No sandboxes to remove' }
$taskSize = (Get-ChildItem -LiteralPath $taskBuild -Recurse -File |
  Measure-Object -Property Length -Sum).Sum / 1MB
('build/ size: {0:N1} MB' -f $taskSize)
