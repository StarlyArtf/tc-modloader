param(
  [switch]$SkipTests,
  [switch]$NoPackage
)

$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskCxx = Join-Path $taskCompiler 'g++.exe'
$taskBuildRoot = Join-Path $taskRoot 'build'
$taskBuild = Join-Path $taskBuildRoot 'float-ops'
$taskStage = Join-Path $taskBuildRoot 'float-ops-package'
$taskDist = Join-Path $taskRoot 'dist'
$taskPackage = Join-Path $taskDist 'local.float-ops.mod'
$taskKernelObjects = Join-Path $taskBuild 'float-kernel-objects.rsp'
$taskKernelIncludes = Join-Path $taskBuild 'float-kernel-includes.rsp'
$taskPreviousEpoch = $env:SOURCE_DATE_EPOCH

if (!(Test-Path -LiteralPath $taskCxx -PathType Leaf)) {
  throw "Missing MinGW compiler: $taskCxx (set TC_MINGW_BIN to override)"
}

function Remove-TaskDirectory([string]$Path) {
  $taskFull = [IO.Path]::GetFullPath($Path)
  $taskAllowed = [IO.Path]::GetFullPath($taskBuildRoot) + [IO.Path]::DirectorySeparatorChar
  if (!$taskFull.StartsWith($taskAllowed, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to remove a directory outside build/: $taskFull"
  }
  if (Test-Path -LiteralPath $taskFull) {
    Remove-Item -LiteralPath $taskFull -Recurse -Force
  }
}

try {
  if (!$env:SOURCE_DATE_EPOCH) { $env:SOURCE_DATE_EPOCH = '315532800' }
  New-Item -ItemType Directory -Force $taskBuild,$taskDist | Out-Null

  if (!$SkipTests) {
    # M1: the vendored SoftFloat subset and the binary32 kernel, built once and
    # shared with the Mod DLL and the true-game drivers.
    & (Join-Path $PSScriptRoot 'build-kernel.ps1')
    if ($LASTEXITCODE) { throw 'Float Ops kernel build failed' }
    $taskObjects = @(Get-Content -LiteralPath $taskKernelObjects)
    $taskKernelFlags = @(Get-Content -LiteralPath $taskKernelIncludes)
    if (!$taskObjects.Count) { throw 'The float kernel object list is empty' }

    $taskKernelTest = Join-Path $taskBuild 'float-kernel-test.exe'
    & $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static @taskKernelFlags `
      (Join-Path $taskRoot 'tests\float-kernel.cpp') $taskObjects -o $taskKernelTest
    if ($LASTEXITCODE) { throw 'Float Ops kernel test did not compile' }
    & $taskKernelTest
    if ($LASTEXITCODE) { throw 'Float Ops kernel test failed' }

    $taskDecimalTest = Join-Path $taskBuild 'float-decimal-test.exe'
    & $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static @taskKernelFlags `
      (Join-Path $taskRoot 'tests\float-decimal.cpp') $taskObjects -o $taskDecimalTest
    if ($LASTEXITCODE) { throw 'Float Ops text-layer test did not compile' }
    & $taskDecimalTest
    if ($LASTEXITCODE) { throw 'Float Ops text-layer test failed' }

    $taskTest = Join-Path $taskBuild 'float-compat-test.exe'
    & $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static @taskKernelFlags `
      (Join-Path $taskRoot 'tests\float-compat.cpp') $taskObjects -o $taskTest
    if ($LASTEXITCODE) { throw 'Float Ops M0 compatibility test did not compile' }
    & $taskTest
    if ($LASTEXITCODE) { throw 'Float Ops M0 compatibility test failed' }
  } else {
    & (Join-Path $PSScriptRoot 'build-kernel.ps1')
    if ($LASTEXITCODE) { throw 'Float Ops kernel build failed' }
  }
  $taskObjects = @(Get-Content -LiteralPath $taskKernelObjects)
  $taskKernelFlags = @(Get-Content -LiteralPath $taskKernelIncludes)

  Remove-TaskDirectory $taskStage
  $taskNative = Join-Path $taskStage 'native'
  $taskLicenses = Join-Path $taskNative 'licenses'
  New-Item -ItemType Directory -Force $taskNative,$taskLicenses | Out-Null

  $taskDll = Join-Path $taskNative 'float-ops.dll'
  & $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror -static -static-libgcc `
    -static-libstdc++ -shared "-I$(Join-Path $taskRoot 'sdk')" `
    @taskKernelFlags `
    (Join-Path $PSScriptRoot 'plugin.cpp') $taskObjects -o $taskDll
  if ($LASTEXITCODE) { throw 'Float Ops M0 plugin did not compile' }

  # The picture each type is shown with in the game's own component column, its
  # drawer preview and the placement ghost (tc.component.render V5).  It is not
  # drawn a second time by hand: the generator includes the Mod's own
  # components.cpp and runs the real draw callback against a rasterising draw
  # table, so the picture is what the part looks like on the board.  The PNGs
  # deploy with the package, which is what gives the plugin an absolute path to
  # hand to the loader.  components.o is left out of that link: the generator's
  # own translation unit *is* components.cpp.
  $taskIconTool = Join-Path $taskBuild 'icons.exe'
  $taskIconObjects = @($taskObjects | Where-Object { $_ -notmatch 'components\.o$' })
  & $taskCxx -std=c++17 -O2 -Wall -Wextra -static @taskKernelFlags `
    (Join-Path $PSScriptRoot 'icons.cpp') $taskIconObjects -o $taskIconTool `
    -lgdiplus -lgdi32 -lole32
  if ($LASTEXITCODE) { throw 'Float Ops icon generator did not compile' }
  $taskIcons = Join-Path $taskStage 'files\asset\float-ops-icons'
  New-Item -ItemType Directory -Force $taskIcons | Out-Null
  & $taskIconTool $taskIcons | Out-Host
  if ($LASTEXITCODE) { throw 'Float Ops icon generation failed' }
  $taskIconCount = @(Get-ChildItem -LiteralPath $taskIcons -Filter '*.png').Count
  if ($taskIconCount -ne 22) {
    throw "Expected the 22 catalogue pictures, the generator wrote $taskIconCount"
  }

  Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'mod.json') -Destination $taskStage
  Copy-Item -LiteralPath (Join-Path $taskRoot 'LICENSE') `
    -Destination (Join-Path $taskLicenses 'tc-modloader-license.txt')
  # Vendored third-party licence (plan 10.3: every borrowed source ships its
  # licence next to the binary).
  Copy-Item -LiteralPath `
    (Join-Path $PSScriptRoot 'third_party\berkeley-softfloat-3\COPYING.txt') `
    -Destination (Join-Path $taskLicenses 'berkeley-softfloat-3.txt')

  if (!$NoPackage) {
    if (Test-Path -LiteralPath $taskPackage) { Remove-Item -LiteralPath $taskPackage -Force }
    & (Join-Path $taskRoot 'tools\Pack-Mod.ps1') -Source $taskStage -Output $taskPackage | Out-Host
    if (!(Test-Path -LiteralPath $taskPackage -PathType Leaf)) {
      throw 'Float Ops package was not created'
    }
  }

  Write-Host 'Float Ops M0 build complete'
  Write-Host "  DLL:     $taskDll"
  if (!$NoPackage) { Write-Host "  Package: $taskPackage" }
} finally {
  $env:SOURCE_DATE_EPOCH = $taskPreviousEpoch
}
