param(
  [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskBuildRoot = Join-Path $taskRepo 'build'
$taskPackage = Join-Path $taskRepo 'dist\local.float-ops.mod'
$taskCli = Join-Path $taskRepo 'dist\tcmod-cli.exe'

if (!$SkipBuild) {
  & (Join-Path $taskRepo 'examples\float-ops\build.ps1')
  if ($LASTEXITCODE) { throw 'Float Ops build failed' }
}
if (!(Test-Path -LiteralPath $taskPackage -PathType Leaf)) {
  throw "Missing package: $taskPackage"
}
if (!(Test-Path -LiteralPath $taskCli -PathType Leaf)) {
  throw "Missing CLI: $taskCli (run the repository build first)"
}

# The kernel test compares against the generated golden vectors, so a generator
# edit without regenerating them would otherwise go unnoticed.
$taskPython = if ($env:TC_PYTHON) { $env:TC_PYTHON } else { 'python' }
& $taskPython (Join-Path $taskRepo 'tools\float-vectors.py') --check
if ($LASTEXITCODE) { throw 'The Float Ops golden vectors are out of date' }

$taskSandbox = Join-Path $taskBuildRoot ('float-ops-cli-smoke-' + [guid]::NewGuid().ToString('N'))
$taskMods = Join-Path $taskSandbox 'mods'
New-Item -ItemType Directory -Force $taskMods | Out-Null
Copy-Item -LiteralPath $taskPackage -Destination (Join-Path $taskMods 'local.float-ops.mod')

$taskListing = & $taskCli $taskSandbox list
if ($LASTEXITCODE) { throw 'Float Ops package list failed' }
if (!(($taskListing -join "`n").Contains('local.float-ops'))) {
  throw 'Float Ops package was not discovered'
}

& $taskCli $taskSandbox apply local.float-ops | Out-Host
if ($LASTEXITCODE) { throw 'Float Ops package apply failed' }
$taskStatePath = Join-Path $taskSandbox 'tc-modloader-data\state.json'
$taskEnabled = Get-Content -LiteralPath $taskStatePath -Raw | ConvertFrom-Json
if (@($taskEnabled.enabled) -notcontains 'local.float-ops') {
  throw 'Float Ops was not enabled in the isolated state'
}
if (@($taskEnabled.native.PSObject.Properties.Name) -notcontains 'local.float-ops') {
  throw 'Float Ops native package digest was not recorded'
}

# The component pictures ship as files/asset/float-ops-icons/<decimal id>.png, so
# enabling the package has to deploy them (and disabling it has to take them away
# again): the plugin hands the loader those very paths.
$taskIcons = Join-Path $taskSandbox 'asset\float-ops-icons'
$taskDeployed = @(Get-ChildItem -LiteralPath $taskIcons -Filter '*.png' -ErrorAction SilentlyContinue)
if ($taskDeployed.Count -ne 22) {
  throw "Expected the 22 component pictures to be deployed, found $($taskDeployed.Count)"
}

& $taskCli $taskSandbox disable-all | Out-Host
if ($LASTEXITCODE) { throw 'Float Ops package disable failed' }
$taskDisabled = Get-Content -LiteralPath $taskStatePath -Raw | ConvertFrom-Json
if (@($taskDisabled.enabled).Count -ne 0) { throw 'Enabled set is not empty after disable-all' }
if (@($taskDisabled.native.PSObject.Properties).Count -ne 0) {
  throw 'Native package state remains after disable-all'
}
$taskLeft = @(Get-ChildItem -LiteralPath $taskIcons -Filter '*.png' -ErrorAction SilentlyContinue)
if ($taskLeft.Count) {
  throw "The component pictures are still deployed after disable-all: $($taskLeft.Count) file(s)"
}

$taskAllowed = @('mods', 'tc-modloader-data', 'asset')
$taskUnexpected = @(Get-ChildItem -LiteralPath $taskSandbox -Force | Where-Object {
  $taskAllowed -notcontains $_.Name
})
if ($taskUnexpected.Count) {
  throw ('Package smoke test wrote outside its Mod/data roots: ' +
         (($taskUnexpected | ForEach-Object Name) -join ', '))
}

"PASS float-ops package: discovered, enabled and disabled in isolation; no Loader/game binary was written"
"Sandbox: $taskSandbox"
