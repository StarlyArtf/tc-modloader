param(
  [switch]$Force
)

# Compiles the vendored SoftFloat subset plus the binary32 kernel into
# build/float-ops/obj and leaves the object list in
# build/float-ops/float-kernel-objects.rsp.
#
# Three consumers share those objects: the Mod DLL, the offline kernel test
# (tests/float-kernel.cpp) and the true-game test drivers.  Keeping one place
# that knows the include paths and the SoftFloat option defines is what stops
# the kernel in the shipped package from drifting away from the kernel the
# tests exercise.

$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskGcc = Join-Path $taskCompiler 'gcc.exe'
$taskCxx = Join-Path $taskCompiler 'g++.exe'
$taskVendor = Join-Path $PSScriptRoot 'third_party\berkeley-softfloat-3'
$taskSoftfloat = Join-Path $taskVendor 'source'
$taskObjectRoot = Join-Path $taskRoot 'build\float-ops\obj'
$taskSoftfloatObjects = Join-Path $taskObjectRoot 'softfloat'
$taskResponseFile = Join-Path $taskRoot 'build\float-ops\float-kernel-objects.rsp'
$taskIncludeFile = Join-Path $taskRoot 'build\float-ops\float-kernel-includes.rsp'
$taskSourceList = Join-Path $PSScriptRoot 'fp\softfloat-sources.txt'

# The option set mirrors build/Win64-MinGW-w64 in the upstream tree; the option
# list and the include layout are documented in fp/softfloat/platform.h and in
# third_party/berkeley-softfloat-3/README.md.
$taskDefines = @(
  '-DSOFTFLOAT_FAST_INT64', '-DSOFTFLOAT_ROUND_ODD', '-DINLINE_LEVEL=5',
  '-DSOFTFLOAT_FAST_DIV32TO16', '-DSOFTFLOAT_FAST_DIV64TO32'
)
$taskIncludes = @(
  "-I$(Join-Path $PSScriptRoot 'fp\softfloat')",
  "-I$(Join-Path $taskSoftfloat 'include')",
  "-I$(Join-Path $taskSoftfloat 'ARM-VFPv2-defaultNaN')"
)

foreach ($taskRequired in @($taskGcc, $taskCxx, $taskSourceList)) {
  if (!(Test-Path -LiteralPath $taskRequired)) { throw "Missing prerequisite: $taskRequired" }
}
New-Item -ItemType Directory -Force $taskSoftfloatObjects | Out-Null

function Test-Stale([string]$Object, [string[]]$Inputs) {
  if ($Force -or !(Test-Path -LiteralPath $Object)) { return $true }
  $taskStamp = (Get-Item -LiteralPath $Object).LastWriteTimeUtc
  foreach ($taskInput in $Inputs) {
    if ((Get-Item -LiteralPath $taskInput).LastWriteTimeUtc -gt $taskStamp) { return $true }
  }
  return $false
}

$taskPlatform = Join-Path $PSScriptRoot 'fp\softfloat\platform.h'
$taskObjects = @()
foreach ($taskLine in Get-Content -LiteralPath $taskSourceList) {
  $taskRelative = $taskLine.Trim()
  if (!$taskRelative -or $taskRelative.StartsWith('#')) { continue }
  # The list records paths relative to the vendored tree root ("source/...").
  $taskSource = Join-Path $taskVendor ($taskRelative -replace '/', '\')
  if (!(Test-Path -LiteralPath $taskSource)) { throw "Missing SoftFloat source: $taskSource" }
  $taskObject = Join-Path $taskSoftfloatObjects `
    ((Split-Path -Leaf $taskSource) -replace '\.c$', '.o')
  if (Test-Stale $taskObject @($taskSource, $taskPlatform)) {
    # Vendored sources are compiled without -Wall/-Werror on purpose: upstream
    # carries warnings (unused parameters in the specialization header, an
    # intentional fallthrough in f32_roundToInt) and this tree does not patch
    # them.  Our own code is compiled with -Wall -Wextra -Werror below.
    & $taskGcc -std=c11 -O2 @taskDefines @taskIncludes -c $taskSource -o $taskObject
    if ($LASTEXITCODE) { throw "SoftFloat object failed: $taskSource" }
  }
  $taskObjects += $taskObject
}

$taskKernelObject = Join-Path $taskObjectRoot 'fp32.o'
$taskKernelSource = Join-Path $PSScriptRoot 'fp\fp32.cpp'
if (Test-Stale $taskKernelObject @($taskKernelSource,
    (Join-Path $PSScriptRoot 'fp\fp32.hpp'),
    (Join-Path $PSScriptRoot 'fp\environment.hpp'), $taskPlatform)) {
  & $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror @taskDefines @taskIncludes `
    -c $taskKernelSource -o $taskKernelObject
  if ($LASTEXITCODE) { throw 'The binary32 kernel did not compile' }
}
$taskObjects += $taskKernelObject

# The text layer (plan 5.3): vendored Ryu plus fp/decimal.cpp.  Ryu is C, so it
# goes through gcc like the SoftFloat subset.
$taskRyuObject = Join-Path $taskObjectRoot 'ryu-f2s.o'
$taskRyuSource = Join-Path $PSScriptRoot 'third_party\ryu\ryu\f2s.c'
if (Test-Stale $taskRyuObject @($taskRyuSource)) {
  & $taskGcc -std=c11 -O2 "-I$(Join-Path $PSScriptRoot 'third_party\ryu')" `
    -c $taskRyuSource -o $taskRyuObject
  if ($LASTEXITCODE) { throw 'Ryu did not compile' }
}
$taskObjects += $taskRyuObject

$taskDecimalObject = Join-Path $taskObjectRoot 'decimal.o'
$taskDecimalSource = Join-Path $PSScriptRoot 'fp\decimal.cpp'
$taskTextFlags = @("-I$(Join-Path $PSScriptRoot 'third_party\fast_float\include')",
                   "-I$(Join-Path $PSScriptRoot 'third_party\ryu')")
if (Test-Stale $taskDecimalObject @($taskDecimalSource,
    (Join-Path $PSScriptRoot 'fp\decimal.hpp'))) {
  & $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror @taskTextFlags `
    -c $taskDecimalSource -o $taskDecimalObject
  if ($LASTEXITCODE) { throw 'The decimal layer did not compile' }
}
$taskObjects += $taskDecimalObject

# The M2 components (plan 6.1 and 7.2).
foreach ($taskComponent in @('components.cpp', 'components_ui.cpp')) {
  $taskComponentObject = Join-Path $taskObjectRoot `
    ((Split-Path -Leaf $taskComponent) -replace '\.cpp$', '.o')
  if (Test-Stale $taskComponentObject @(
      (Join-Path $PSScriptRoot $taskComponent),
      (Join-Path $PSScriptRoot 'components.hpp'),
      (Join-Path $PSScriptRoot 'components_internal.hpp'),
      $taskKernelSource, $taskDecimalSource)) {
    & $taskCxx -std=c++17 -O2 -Wall -Wextra -Werror @taskDefines @taskIncludes `
      @taskTextFlags "-I$(Join-Path $taskRoot 'sdk')" `
      -c (Join-Path $PSScriptRoot $taskComponent) -o $taskComponentObject
    if ($LASTEXITCODE) { throw "$taskComponent did not compile" }
  }
  $taskObjects += $taskComponentObject
}

Set-Content -LiteralPath $taskResponseFile -Value $taskObjects -Encoding ascii
# The include set has to travel with the objects: anything that includes
# fp/fp32.hpp compiles against the same platform.h and SoftFloat headers.
Set-Content -LiteralPath $taskIncludeFile `
  -Value (@($taskDefines) + @($taskIncludes) + @($taskTextFlags) +
          @("-I$(Join-Path $taskRoot 'sdk')")) -Encoding ascii

Write-Host ("Float Ops kernel and text objects ready: {0} object(s) -> {1}" -f `
            ($taskObjects.Count - 1), $taskObjectRoot)
