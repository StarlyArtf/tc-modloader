$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
if ((Get-Item -LiteralPath (Join-Path $taskRepo 'dist\TCModLoader-Setup.exe')).VersionInfo.FileDescription -cne
    ('TC Mod Loader ' + (-join @([char]0x5b89, [char]0x88c5, [char]0x5668)))) {
  throw 'Installer file description was corrupted by script encoding'
}
Import-Module (Join-Path $taskRepo 'tools\ReleaseTools.psm1') -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Assert-PeTimestampBytes([byte[]]$Bytes, [uint32]$Expected, [string]$Name) {
  if ($Bytes.Length -lt 256 -or $Bytes[0] -ne 0x4d -or $Bytes[1] -ne 0x5a) { throw "Not a PE file: $Name" }
  $taskPe = [BitConverter]::ToInt32($Bytes, 0x3c)
  if ($taskPe -lt 0 -or $taskPe + 12 -gt $Bytes.Length -or
      $Bytes[$taskPe] -ne 0x50 -or $Bytes[$taskPe + 1] -ne 0x45) { throw "Invalid PE header: $Name" }
  $taskTimestamp = [BitConverter]::ToUInt32($Bytes, $taskPe + 8)
  if ($taskTimestamp -ne $Expected) {
    throw "Non-deterministic PE timestamp in $Name`: expected $Expected, found $taskTimestamp"
  }
}

function Assert-PeTimestampFile([string]$Path, [uint32]$Expected) {
  Assert-PeTimestampBytes ([IO.File]::ReadAllBytes($Path)) $Expected $Path
}

$taskBuild = Join-Path $taskRepo 'build'
$taskFixture = Join-Path $taskBuild ('release-contract-' + [guid]::NewGuid().ToString('N'))
$taskSource = Join-Path $taskFixture 'source'
$taskNative = Join-Path $taskSource 'native'
New-Item -ItemType Directory -Force $taskNative | Out-Null
try {
  [IO.File]::WriteAllText(
    (Join-Path $taskSource 'mod.json'),
    '{"format":2,"id":"test.release","name":"Release fixture","version":"1.0.0","native":{"api":1,"entry":"native/plugin.dll"}}',
    [Text.Encoding]::ASCII)
  [IO.File]::WriteAllBytes((Join-Path $taskNative 'plugin.dll'), [byte[]](0, 1, 2, 3, 255))
  [IO.File]::WriteAllText((Join-Path $taskNative 'z-last.txt'), 'z', [Text.Encoding]::ASCII)
  [IO.File]::WriteAllText((Join-Path $taskNative 'a-first.txt'), 'a', [Text.Encoding]::ASCII)

  $taskFirst = Join-Path $taskFixture 'first.mod'
  $taskSecond = Join-Path $taskFixture 'second.mod'
  & (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskSource -Output $taskFirst | Out-Null
  if ($LASTEXITCODE) { throw 'First deterministic Mod pack failed' }
  & (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskSource -Output $taskSecond | Out-Null
  if ($LASTEXITCODE) { throw 'Second deterministic Mod pack failed' }
  $taskFirstHash = (Get-FileHash -LiteralPath $taskFirst -Algorithm SHA256).Hash
  $taskSecondHash = (Get-FileHash -LiteralPath $taskSecond -Algorithm SHA256).Hash
  if ($taskFirstHash -cne $taskSecondHash) { throw 'Identical Mod sources produced different archives' }
  $taskFixtureInfo = Read-TcModArchive -Package $taskFirst -ExpectedId 'test.release' -RequireDeterministic
  if ($taskFixtureInfo.Entries -ne 4) { throw "Unexpected deterministic fixture entry count: $($taskFixtureInfo.Entries)" }

  $taskReleaseManifest = Get-Content -LiteralPath (Join-Path $taskRepo 'release\manifest.json') -Raw | ConvertFrom-Json
  if ([int]$taskReleaseManifest.schemaVersion -ne 1) { throw 'Unexpected release manifest schema' }
  # A source release must carry the inputs needed by build.ps1, including the
  # simulator headers/viewer and the installer's icon, rather than depending on
  # files that only exist in the developer's checkout.
  foreach ($taskRequiredTree in @('simcore', 'assets')) {
    if (@($taskReleaseManifest.sourceArchive.trees) -notcontains $taskRequiredTree) {
      throw "Source release is missing a build input tree: $taskRequiredTree"
    }
  }
  foreach ($taskRequiredFile in @('build-text-box.ps1', 'build-wire-palette.ps1')) {
    if (@($taskReleaseManifest.sourceArchive.files) -notcontains $taskRequiredFile) {
      throw "Source release is missing an example build script: $taskRequiredFile"
    }
  }
  $taskTargets = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
  foreach ($taskEntry in @($taskReleaseManifest.playerArchive.files) +
      @($taskReleaseManifest.playerArchive.licenses) + @($taskReleaseManifest.playerArchive.mods)) {
    if (!$taskTargets.Add([string]$taskEntry.target)) { throw "Duplicate explicit player target: $($taskEntry.target)" }
  }
  $taskExpectedEpoch = [uint32]$(if ($env:SOURCE_DATE_EPOCH) { [long]$env:SOURCE_DATE_EPOCH } else { 315532800 })
  foreach ($taskMod in $taskReleaseManifest.playerArchive.mods) {
    $taskModPath = Join-Path $taskRepo ([string]$taskMod.source)
    Read-TcModArchive -Package $taskModPath `
      -ExpectedId ([string]$taskMod.id) -RequireDeterministic | Out-Null
    $taskZip = [IO.Compression.ZipFile]::OpenRead($taskModPath)
    try {
      foreach ($taskDll in $taskZip.Entries | Where-Object { $_.FullName -like 'native/*.dll' }) {
        $taskMemory = New-Object IO.MemoryStream
        $taskStream = $taskDll.Open()
        try { $taskStream.CopyTo($taskMemory) } finally { $taskStream.Dispose() }
        try { Assert-PeTimestampBytes ($taskMemory.ToArray()) $taskExpectedEpoch ($taskMod.id + '/' + $taskDll.FullName) }
        finally { $taskMemory.Dispose() }
      }
    } finally { $taskZip.Dispose() }
  }
  foreach ($taskBinary in @('dist\tc-loader.dll', 'dist\tcmod-cli.exe', 'dist\TCModLoader-Setup.exe')) {
    Assert-PeTimestampFile (Join-Path $taskRepo $taskBinary) $taskExpectedEpoch
  }
  # The installer is a pair with the loader it deploys: the release refuses to
  # stage an installer embedding some other build, or one whose version block no
  # longer matches VERSION.  This is the same check tools/release.ps1 runs before
  # it stages anything, so the fast tier catches it first.
  $taskInstaller = Assert-TcInstallerMatchesRelease -Repo $taskRepo
  "PASS release contract: the installer embeds dist\tc-loader.dll " +
    "($($taskInstaller.LoaderSha256.Substring(0, 8))...) and carries version $($taskInstaller.Version)"
  $taskPlan = & (Join-Path $taskRepo 'tools\release.ps1') -Plan
  $taskPlanText = $taskPlan -join "`n"
  # The archive name has to follow VERSION, so this reads it instead of pinning
  # one release: the hardcoded "0.6.0" turned a version bump into a failing
  # release contract.
  $taskVersion = (Get-Content -LiteralPath (Join-Path $taskRepo 'VERSION') -Raw).Trim()
  if ($LASTEXITCODE -or $taskPlanText -notmatch ('TCModLoader-'+[regex]::Escape($taskVersion)+'-win64\.zip') -or
      $taskPlanText -notmatch 'Compatibility profile: tc-win64-2\.1\.334' -or
      $taskPlanText -notmatch 'SDK ABI target: windows-x64-mingw-ucrt') {
    throw 'Release plan did not resolve the current version, compatibility profile and ABI target'
  }
  "PASS release contract: deterministic Mod hash $($taskFirstHash.ToLowerInvariant()), manifest targets unique, published Mods valid"
} finally {
  if (Test-Path -LiteralPath $taskFixture) {
    if (!(Test-TcChildPath -Parent $taskBuild -Child $taskFixture)) { throw "Refusing to clean outside build: $taskFixture" }
    Remove-Item -LiteralPath $taskFixture -Recurse -Force
  }
}
