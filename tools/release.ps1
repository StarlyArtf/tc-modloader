param(
  [switch]$UseExistingBuild,
  [switch]$SkipTests,
  [ValidateSet('fast', 'host', 'game', 'all')]
  [string]$TestTier = 'game',
  [switch]$AllowDirty,
  [switch]$Force,
  [switch]$KeepStage,
  [switch]$Plan,
  [string]$OutputRoot = ''
)

$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskManifestPath = Join-Path $taskRepo 'release\manifest.json'
Import-Module (Join-Path $PSScriptRoot 'ReleaseTools.psm1') -Force

function Test-ObjectProperty([object]$Object, [string]$Name) {
  return $null -ne $Object.PSObject.Properties[$Name]
}

function Assert-SafeRelativePath([string]$Path, [string]$What) {
  if (!$Path -or [IO.Path]::IsPathRooted($Path) -or $Path.Contains('..') -or $Path.Contains(':')) {
    throw "Unsafe $What path in release manifest: $Path"
  }
}

function Resolve-ReleaseName([string]$Template, [string]$Version) {
  if (!$Template.Contains('{version}')) { throw "Release archive name must contain {version}: $Template" }
  return $Template.Replace('{version}', $Version)
}

function Invoke-ReleaseTest([string[]]$Arguments) {
  & (Join-Path $taskRepo 'tools\test.ps1') @Arguments
  if ($LASTEXITCODE) { throw "Release validation failed: tools/test.ps1 $($Arguments -join ' ')" }
}

function Test-Excluded([string]$RelativePath, [object[]]$Patterns) {
  foreach ($taskPattern in $Patterns) {
    if ($RelativePath -like [string]$taskPattern) { return $true }
  }
  return $false
}

function Add-StageFile {
  param(
    [Parameter(Mandatory = $true)][string]$Stage,
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Target,
    [Parameter(Mandatory = $true)]$Targets
  )
  Assert-SafeRelativePath $Target 'target'
  $taskSourcePath = [IO.Path]::GetFullPath((Join-Path $taskRepo $Source))
  if (!(Test-TcChildPath -Parent $taskRepo -Child $taskSourcePath -AllowEqual)) {
    throw "Release source escapes the repository: $Source"
  }
  if (!(Test-Path -LiteralPath $taskSourcePath -PathType Leaf)) { throw "Release source is missing: $Source" }
  $taskTargetKey = $Target.Replace('\', '/')
  if (!$Targets.Add($taskTargetKey)) { throw "Duplicate release target: $taskTargetKey" }
  $taskTargetPath = Join-Path $Stage $Target
  $taskParent = Split-Path $taskTargetPath
  if ($taskParent) { New-Item -ItemType Directory -Force $taskParent | Out-Null }
  Copy-Item -LiteralPath $taskSourcePath -Destination $taskTargetPath
}

function Add-StageTree {
  param(
    [Parameter(Mandatory = $true)][string]$Stage,
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Target,
    [Parameter(Mandatory = $true)]$Targets,
    [object[]]$Exclude = @()
  )
  Assert-SafeRelativePath $Source 'tree source'
  if ($Target) { Assert-SafeRelativePath $Target 'tree target' }
  $taskTreePath = [IO.Path]::GetFullPath((Join-Path $taskRepo $Source))
  if (!(Test-TcChildPath -Parent $taskRepo -Child $taskTreePath -AllowEqual) -or
      !(Test-Path -LiteralPath $taskTreePath -PathType Container)) {
    throw "Release source tree is missing or outside the repository: $Source"
  }
  foreach ($taskFile in Get-ChildItem -LiteralPath $taskTreePath -Recurse -File) {
    $taskRelative = $taskFile.FullName.Substring($taskRepo.Length).TrimStart('\', '/').Replace('\', '/')
    if (Test-Excluded $taskRelative $Exclude) { continue }
    $taskWithinTree = $taskFile.FullName.Substring($taskTreePath.Length).TrimStart('\', '/')
    $taskTarget = if ($Target) { Join-Path $Target $taskWithinTree } else { $taskWithinTree }
    Add-StageFile -Stage $Stage -Source $taskRelative -Target $taskTarget -Targets $Targets
  }
}

function Write-HashList([string]$Directory, [string]$Output, [string[]]$OnlyNames = @()) {
  $taskFiles = if ($OnlyNames.Count) {
    @($OnlyNames | ForEach-Object { Get-Item -LiteralPath (Join-Path $Directory $_) })
  } else {
    @(Get-ChildItem -LiteralPath $Directory -Recurse -File | Where-Object { $_.FullName -ne [IO.Path]::GetFullPath($Output) })
  }
  $taskRows = @()
  foreach ($taskFile in $taskFiles) {
    $taskRelative = $taskFile.FullName.Substring([IO.Path]::GetFullPath($Directory).TrimEnd('\').Length).TrimStart('\').Replace('\', '/')
    $taskRows += [pscustomobject]@{ Relative = $taskRelative; File = $taskFile }
  }
  $taskMap = @{}
  foreach ($taskRow in $taskRows) { $taskMap[$taskRow.Relative] = $taskRow }
  [string[]]$taskNames = @($taskMap.Keys)
  [Array]::Sort($taskNames, [StringComparer]::Ordinal)
  $taskLines = foreach ($taskName in $taskNames) {
    (Get-FileHash -LiteralPath $taskMap[$taskName].File.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $taskName
  }
  [IO.File]::WriteAllText($Output, (($taskLines -join "`n") + "`n"), [Text.Encoding]::ASCII)
}

$taskManifest = Get-Content -LiteralPath $taskManifestPath -Raw | ConvertFrom-Json
if ([int]$taskManifest.schemaVersion -ne 1) { throw "Unsupported release manifest schema: $($taskManifest.schemaVersion)" }
foreach ($taskRequired in @('product', 'versionFile', 'sourceArchive', 'playerArchive', 'standalone')) {
  if (!(Test-ObjectProperty $taskManifest $taskRequired)) { throw "Release manifest is missing '$taskRequired'" }
}

$taskVersionPath = Join-Path $taskRepo ([string]$taskManifest.versionFile)
$taskVersion = (Get-Content -LiteralPath $taskVersionPath -Raw).Trim()
if ($taskVersion -notmatch '^\d+\.\d+\.\d+$') { throw "Release version must be MAJOR.MINOR.PATCH: $taskVersion" }
$taskSourceName = Resolve-ReleaseName ([string]$taskManifest.sourceArchive.name) $taskVersion
$taskPlayerName = Resolve-ReleaseName ([string]$taskManifest.playerArchive.name) $taskVersion
$taskCompatibilityPath = Join-Path $taskRepo 'compat\profiles.json'
$taskCompatibility = Get-Content -LiteralPath $taskCompatibilityPath -Raw | ConvertFrom-Json
$taskCurrentProfiles = @($taskCompatibility.profiles | Where-Object { $_.current -eq $true })
if ($taskCurrentProfiles.Count -ne 1) { throw 'Release requires exactly one current compatibility profile' }
$taskCompatibilityProfile = $taskCurrentProfiles[0]
if ([string]$taskCompatibilityProfile.status -ne 'supported') {
  throw "Release requires a supported current compatibility profile; $($taskCompatibilityProfile.id) is $($taskCompatibilityProfile.status)"
}
$taskAbiBaselinePath = Join-Path $taskRepo 'abi\windows-x64.json'
if (!(Test-Path -LiteralPath $taskAbiBaselinePath -PathType Leaf)) { throw 'Release requires the Windows x64 SDK ABI baseline' }
$taskAbiBaseline = Get-Content -LiteralPath $taskAbiBaselinePath -Raw | ConvertFrom-Json
if ([int]$taskAbiBaseline.schemaVersion -ne 1 -or ![string]$taskAbiBaseline.target) { throw 'Release requires a valid SDK ABI baseline' }

if (!$OutputRoot) { $OutputRoot = Join-Path $taskRepo 'dist\releases' }
$taskOutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$taskReleaseDirectory = Join-Path $taskOutputRoot $taskVersion

if ($Plan) {
  "Product: $($taskManifest.product)"
  "Version: $taskVersion"
  "Compatibility profile: $($taskCompatibilityProfile.id)"
  "SDK ABI target: $($taskAbiBaseline.target)"
  "Source archive: $taskSourceName"
  "Player archive: $taskPlayerName"
  "Release directory: $taskReleaseDirectory"
  "Published Mods:"
  foreach ($taskMod in $taskManifest.playerArchive.mods) { "  $($taskMod.id) <- $($taskMod.source)" }
  exit 0
}

$taskGit = Get-Command git.exe -ErrorAction SilentlyContinue
$taskDirty = $false
$taskCommit = 'unknown'
if ($taskGit) {
  $taskCommit = (& $taskGit.Source -C $taskRepo rev-parse HEAD).Trim()
  if ($LASTEXITCODE) { throw 'Could not read the source commit' }
  $taskStatus = @(& $taskGit.Source -C $taskRepo status --porcelain=v1 --untracked-files=all)
  $taskDirty = $taskStatus.Count -gt 0
  if ($taskDirty -and !$AllowDirty) {
    throw 'Release refused: the working tree is dirty. Commit/stash it, or pass -AllowDirty for a local non-publishable build.'
  }
}

if (!$SkipTests) {
  if ($UseExistingBuild) {
    foreach ($taskFastTest in @('compatibility-contract', 'sdk-abi', 'package-manager', 'save-isolation', 'release-contract')) {
      Invoke-ReleaseTest @('-Name', $taskFastTest, '-NoBuild')
    }
  } else {
    Invoke-ReleaseTest @('-Tier', 'fast')
  }
  if ($TestTier -in @('host', 'game', 'all')) { Invoke-ReleaseTest @('-Tier', 'host', '-NoBuild') }
  if ($TestTier -in @('game', 'all')) { Invoke-ReleaseTest @('-Tier', 'game', '-NoBuild') }
  if ($TestTier -eq 'all') { Invoke-ReleaseTest @('-Name', '*-diagnostic', '-NoBuild', '-KeepGoing') }
} elseif (!$UseExistingBuild) {
  & (Join-Path $taskRepo 'build.ps1')
  if ($LASTEXITCODE) { throw 'Release build failed' }
}

if (!$AllowDirty -and $taskGit) {
  $taskAfterBuild = @(& $taskGit.Source -C $taskRepo status --porcelain=v1 --untracked-files=all)
  if ($taskAfterBuild.Count) { throw 'Build changed the clean working tree; release refused.' }
}

$taskWork = Join-Path $taskRepo ('build\release-work-' + [guid]::NewGuid().ToString('N'))
$taskSourceStage = Join-Path $taskWork 'source'
$taskPlayerStage = Join-Path $taskWork 'player'
$taskArtifacts = Join-Path $taskWork 'artifacts'
New-Item -ItemType Directory -Force $taskSourceStage, $taskPlayerStage, $taskArtifacts | Out-Null

try {
  $taskSourceTargets = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
  foreach ($taskFile in $taskManifest.sourceArchive.files) {
    Add-StageFile -Stage $taskSourceStage -Source ([string]$taskFile) -Target ([string]$taskFile) -Targets $taskSourceTargets
  }
  foreach ($taskTree in $taskManifest.sourceArchive.trees) {
    Add-StageTree -Stage $taskSourceStage -Source ([string]$taskTree) -Target ([string]$taskTree) `
      -Targets $taskSourceTargets -Exclude @($taskManifest.sourceArchive.exclude)
  }

  $taskSourceArchive = Join-Path $taskArtifacts $taskSourceName
  New-TcDeterministicZip -SourceDirectory $taskSourceStage -Output $taskSourceArchive | Out-Null
  $taskSourceVerify = Join-Path $taskWork ('verify-' + $taskSourceName)
  New-TcDeterministicZip -SourceDirectory $taskSourceStage -Output $taskSourceVerify | Out-Null
  if ((Get-FileHash $taskSourceArchive).Hash -cne (Get-FileHash $taskSourceVerify).Hash) {
    throw 'Source archive reproducibility check failed'
  }
  Remove-Item -LiteralPath $taskSourceVerify -Force

  $taskPlayerTargets = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
  foreach ($taskFile in @($taskManifest.playerArchive.files) + @($taskManifest.playerArchive.licenses)) {
    Add-StageFile -Stage $taskPlayerStage -Source ([string]$taskFile.source) -Target ([string]$taskFile.target) -Targets $taskPlayerTargets
  }
  foreach ($taskTree in $taskManifest.playerArchive.trees) {
    $taskTreeExclude = if (Test-ObjectProperty $taskTree 'exclude') { @($taskTree.exclude) } else { @() }
    Add-StageTree -Stage $taskPlayerStage -Source ([string]$taskTree.source) -Target ([string]$taskTree.target) -Targets $taskPlayerTargets `
      -Exclude $taskTreeExclude
  }
  foreach ($taskMod in $taskManifest.playerArchive.mods) {
    Read-TcModArchive -Package (Join-Path $taskRepo ([string]$taskMod.source)) `
      -ExpectedId ([string]$taskMod.id) -RequireDeterministic | Out-Null
    Add-StageFile -Stage $taskPlayerStage -Source ([string]$taskMod.source) -Target ([string]$taskMod.target) -Targets $taskPlayerTargets
  }
  Add-StageFile -Stage $taskPlayerStage -Source $taskSourceArchive.Substring($taskRepo.Length).TrimStart('\') `
    -Target $taskSourceName -Targets $taskPlayerTargets

  $taskPlayerSums = Join-Path $taskPlayerStage 'SHA256SUMS.txt'
  Write-HashList -Directory $taskPlayerStage -Output $taskPlayerSums
  [void]$taskPlayerTargets.Add('SHA256SUMS.txt')

  $taskPlayerArchive = Join-Path $taskArtifacts $taskPlayerName
  New-TcDeterministicZip -SourceDirectory $taskPlayerStage -Output $taskPlayerArchive | Out-Null
  $taskPlayerVerify = Join-Path $taskWork ('verify-' + $taskPlayerName)
  New-TcDeterministicZip -SourceDirectory $taskPlayerStage -Output $taskPlayerVerify | Out-Null
  if ((Get-FileHash $taskPlayerArchive).Hash -cne (Get-FileHash $taskPlayerVerify).Hash) {
    throw 'Player archive reproducibility check failed'
  }
  Remove-Item -LiteralPath $taskPlayerVerify -Force

  if (Test-Path -LiteralPath $taskReleaseDirectory) {
    if (!$Force) { throw "Release directory already exists: $taskReleaseDirectory (pass -Force to replace this exact version)" }
    if (!(Test-TcChildPath -Parent $taskOutputRoot -Child $taskReleaseDirectory)) {
      throw "Refusing to replace a release outside the configured output root: $taskReleaseDirectory"
    }
    Remove-Item -LiteralPath $taskReleaseDirectory -Recurse -Force
  }
  New-Item -ItemType Directory -Force $taskReleaseDirectory | Out-Null
  Copy-Item -LiteralPath $taskSourceArchive, $taskPlayerArchive -Destination $taskReleaseDirectory
  foreach ($taskStandalone in $taskManifest.standalone) {
    Add-StageFile -Stage $taskReleaseDirectory -Source ([string]$taskStandalone.source) `
      -Target ([string]$taskStandalone.name) `
      -Targets (New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase))
  }

  $taskArtifactFiles = @(Get-ChildItem -LiteralPath $taskReleaseDirectory -File)
  $taskArtifactRows = @()
  foreach ($taskArtifact in ($taskArtifactFiles | Sort-Object Name)) {
    $taskArtifactRows += [ordered]@{
      name = $taskArtifact.Name
      size = $taskArtifact.Length
      sha256 = (Get-FileHash -LiteralPath $taskArtifact.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
  }
  $taskReleaseMetadata = [ordered]@{
    schemaVersion = 1
    product = [string]$taskManifest.product
    version = $taskVersion
    sourceCommit = $taskCommit
    dirty = $taskDirty
    sourceDateEpoch = if ($env:SOURCE_DATE_EPOCH) { [long]$env:SOURCE_DATE_EPOCH } else { 315532800 }
    manifestSha256 = (Get-FileHash -LiteralPath $taskManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    compatibilityProfile = [string]$taskCompatibilityProfile.id
    compatibilityManifestSha256 = (Get-FileHash -LiteralPath $taskCompatibilityPath -Algorithm SHA256).Hash.ToLowerInvariant()
    sdkAbiTarget = [string]$taskAbiBaseline.target
    sdkAbiBaselineSha256 = (Get-FileHash -LiteralPath $taskAbiBaselinePath -Algorithm SHA256).Hash.ToLowerInvariant()
    artifacts = $taskArtifactRows
  }
  $taskMetadataPath = Join-Path $taskReleaseDirectory 'release.json'
  $taskMetadataText = $taskReleaseMetadata | ConvertTo-Json -Depth 5
  [IO.File]::WriteAllText($taskMetadataPath, ($taskMetadataText.Replace("`r`n", "`n") + "`n"), [Text.UTF8Encoding]::new($false))
  $taskFinalNames = @((Get-ChildItem -LiteralPath $taskReleaseDirectory -File | ForEach-Object Name) | Where-Object { $_ -ne 'SHA256SUMS.txt' })
  Write-HashList -Directory $taskReleaseDirectory -Output (Join-Path $taskReleaseDirectory 'SHA256SUMS.txt') -OnlyNames $taskFinalNames

  "Release ready: $taskReleaseDirectory"
  Get-ChildItem -LiteralPath $taskReleaseDirectory -File | Sort-Object Name | Select-Object Name, Length
} finally {
  if ($KeepStage) { "Release staging kept: $taskWork" }
  elseif (Test-Path -LiteralPath $taskWork) {
    $taskBuildRoot = Join-Path $taskRepo 'build'
    if (!(Test-TcChildPath -Parent $taskBuildRoot -Child $taskWork)) {
      throw "Refusing to clean release staging outside build: $taskWork"
    }
    Remove-Item -LiteralPath $taskWork -Recurse -Force
  }
}
