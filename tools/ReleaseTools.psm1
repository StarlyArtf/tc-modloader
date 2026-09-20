Set-StrictMode -Version 2.0

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-TcFullPath {
  param([Parameter(Mandatory = $true)][string]$Path)
  return [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
}

function Test-TcChildPath {
  param(
    [Parameter(Mandatory = $true)][string]$Parent,
    [Parameter(Mandatory = $true)][string]$Child,
    [switch]$AllowEqual
  )
  $taskParent = Get-TcFullPath $Parent
  $taskChild = Get-TcFullPath $Child
  if ($AllowEqual -and $taskChild.Equals($taskParent, [StringComparison]::OrdinalIgnoreCase)) { return $true }
  return ($taskChild + [IO.Path]::DirectorySeparatorChar).StartsWith(
    $taskParent + [IO.Path]::DirectorySeparatorChar,
    [StringComparison]::OrdinalIgnoreCase)
}

function Get-TcArchiveTimestamp {
  [CmdletBinding()]
  param([string]$SourceDateEpoch = $env:SOURCE_DATE_EPOCH)
  if (!$SourceDateEpoch) { $SourceDateEpoch = '315532800' } # 1980-01-01, ZIP's minimum date.
  try { $taskSeconds = [long]::Parse($SourceDateEpoch, [Globalization.CultureInfo]::InvariantCulture) }
  catch { throw "SOURCE_DATE_EPOCH must be an integer number of seconds: $SourceDateEpoch" }
  $taskTimestamp = [DateTimeOffset]::FromUnixTimeSeconds($taskSeconds).ToUniversalTime()
  if ($taskTimestamp.Year -lt 1980 -or $taskTimestamp.Year -gt 2107) {
    throw "SOURCE_DATE_EPOCH is outside the ZIP timestamp range: $SourceDateEpoch"
  }
  # ZIP timestamps have two-second precision.  Normalising here makes the intent explicit.
  return $taskTimestamp.AddSeconds(-($taskTimestamp.Second % 2)).AddTicks(-($taskTimestamp.Ticks % [TimeSpan]::TicksPerSecond))
}

function Test-TcSafeArchivePath {
  param([Parameter(Mandatory = $true)][string]$Path)
  if (!$Path -or $Path.Contains('\') -or $Path.StartsWith('/') -or $Path.Contains(':')) { return $false }
  foreach ($taskPart in $Path.Split('/')) {
    if (!$taskPart -or $taskPart -eq '.' -or $taskPart -eq '..') { return $false }
  }
  return $true
}

function New-TcDeterministicZip {
  [CmdletBinding()]
  param(
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [Parameter(Mandatory = $true)][string]$Output,
    [DateTimeOffset]$Timestamp = (Get-TcArchiveTimestamp)
  )

  $taskSource = Get-TcFullPath $SourceDirectory
  $taskOutput = [IO.Path]::GetFullPath($Output)
  if (!(Test-Path -LiteralPath $taskSource -PathType Container)) { throw "Archive source does not exist: $taskSource" }
  if (Test-TcChildPath -Parent $taskSource -Child $taskOutput -AllowEqual) {
    throw "Archive output must be outside its source directory: $taskOutput"
  }
  if (Test-Path -LiteralPath $taskOutput) { throw "Archive output already exists: $taskOutput" }
  $taskParent = Split-Path $taskOutput
  if ($taskParent) { New-Item -ItemType Directory -Force $taskParent | Out-Null }

  $taskItems = @()
  $taskSeen = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
  foreach ($taskFile in Get-ChildItem -LiteralPath $taskSource -Recurse -File) {
    $taskRelative = $taskFile.FullName.Substring($taskSource.Length).TrimStart('\', '/')
    $taskEntry = $taskRelative.Replace('\', '/')
    if (!(Test-TcSafeArchivePath $taskEntry)) { throw "Unsafe archive path: $taskEntry" }
    if (!$taskSeen.Add($taskEntry)) { throw "Case-insensitive archive path collision: $taskEntry" }
    $taskItems += [pscustomobject]@{ File = $taskFile; Entry = $taskEntry }
  }
  $taskItemMap = @{}
  foreach ($taskItem in $taskItems) { $taskItemMap[[string]$taskItem.Entry] = $taskItem }
  [string[]]$taskEntryNames = @($taskItemMap.Keys)
  [Array]::Sort($taskEntryNames, [StringComparer]::Ordinal)
  $taskItems = @($taskEntryNames | ForEach-Object { $taskItemMap[$_] })

  $taskStream = [IO.File]::Open($taskOutput, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
  try {
    $taskArchive = [IO.Compression.ZipArchive]::new(
      $taskStream,
      [IO.Compression.ZipArchiveMode]::Create,
      $false,
      [Text.UTF8Encoding]::new($false))
    try {
      foreach ($taskItem in $taskItems) {
        $taskEntry = $taskArchive.CreateEntry($taskItem.Entry, [IO.Compression.CompressionLevel]::Optimal)
        $taskEntry.LastWriteTime = $Timestamp
        $taskEntry.ExternalAttributes = 0
        $taskInput = [IO.File]::OpenRead($taskItem.File.FullName)
        $taskOutputStream = $taskEntry.Open()
        try { $taskInput.CopyTo($taskOutputStream) }
        finally { $taskOutputStream.Dispose(); $taskInput.Dispose() }
      }
    } finally { $taskArchive.Dispose() }
  } catch {
    $taskStream.Dispose()
    Remove-Item -LiteralPath $taskOutput -Force -ErrorAction SilentlyContinue
    throw
  } finally {
    if ($taskStream) { $taskStream.Dispose() }
  }
  return Get-Item -LiteralPath $taskOutput
}

function Read-TcModArchive {
  [CmdletBinding()]
  param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$ExpectedId = '',
    [switch]$RequireDeterministic
  )

  $taskPackage = (Resolve-Path -LiteralPath $Package).Path
  $taskStream = [IO.File]::OpenRead($taskPackage)
  try {
    $taskArchive = [IO.Compression.ZipArchive]::new($taskStream, [IO.Compression.ZipArchiveMode]::Read, $false)
    try {
      if ($taskArchive.Entries.Count -lt 1 -or $taskArchive.Entries.Count -gt 4096) {
        throw "Mod archive entry count is invalid: $($taskArchive.Entries.Count)"
      }
      $taskSeen = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
      $taskNames = @()
      $taskManifestEntry = $null
      $taskExpectedTimestamp = Get-TcArchiveTimestamp
      foreach ($taskEntry in $taskArchive.Entries) {
        $taskName = [string]$taskEntry.FullName
        if (!(Test-TcSafeArchivePath $taskName)) { throw "Unsafe path in Mod archive: $taskName" }
        if (!$taskSeen.Add($taskName)) { throw "Duplicate path in Mod archive: $taskName" }
        if ($taskName -ne 'mod.json' -and !$taskName.StartsWith('files/') -and !$taskName.StartsWith('native/')) {
          throw "Unsupported top-level path in Mod archive: $taskName"
        }
        if ($RequireDeterministic -and
            $taskEntry.LastWriteTime.ToString('yyyyMMddHHmmss') -ne $taskExpectedTimestamp.ToString('yyyyMMddHHmmss')) {
          throw "Non-deterministic timestamp in Mod archive: $taskName"
        }
        $taskNames += $taskName
        if ($taskName -eq 'mod.json') { $taskManifestEntry = $taskEntry }
      }
      if (!$taskManifestEntry) { throw 'Mod archive does not contain root mod.json' }
      if ($RequireDeterministic) {
        [string[]]$taskSorted = @($taskNames)
        [Array]::Sort($taskSorted, [StringComparer]::Ordinal)
        if (($taskNames -join "`n") -cne ($taskSorted -join "`n")) { throw 'Mod archive entries are not sorted' }
      }
      $taskReader = New-Object IO.StreamReader($taskManifestEntry.Open(), [Text.Encoding]::UTF8, $true)
      try { $taskManifest = $taskReader.ReadToEnd() | ConvertFrom-Json }
      finally { $taskReader.Dispose() }
      if ([int]$taskManifest.format -notin @(1, 2)) { throw "Unsupported Mod format: $($taskManifest.format)" }
      foreach ($taskField in @('id', 'name', 'version')) {
        if (!$taskManifest.PSObject.Properties[$taskField] -or ![string]$taskManifest.$taskField) {
          throw "Mod manifest is missing '$taskField'"
        }
      }
      if ($ExpectedId -and [string]$taskManifest.id -cne $ExpectedId) {
        throw "Mod id mismatch: expected '$ExpectedId', found '$($taskManifest.id)'"
      }
      if ([int]$taskManifest.format -eq 2) {
        if (!$taskManifest.native -or [int]$taskManifest.native.api -ne 1 -or ![string]$taskManifest.native.entry) {
          throw 'Native Mod must declare native.api=1 and native.entry'
        }
        $taskNativeEntry = ([string]$taskManifest.native.entry).Replace('\', '/')
        if (!(Test-TcSafeArchivePath $taskNativeEntry) -or !$taskNativeEntry.StartsWith('native/')) {
          throw "Unsafe native entry: $taskNativeEntry"
        }
        if (!$taskSeen.Contains($taskNativeEntry)) { throw "Native entry is missing from package: $taskNativeEntry" }
      }
      return [pscustomobject]@{
        Path = $taskPackage
        Id = [string]$taskManifest.id
        Name = [string]$taskManifest.name
        Version = [string]$taskManifest.version
        Format = [int]$taskManifest.format
        Entries = $taskNames.Count
      }
    } finally { $taskArchive.Dispose() }
  } finally { $taskStream.Dispose() }
}

Export-ModuleMember -Function Get-TcArchiveTimestamp, New-TcDeterministicZip, Read-TcModArchive, Test-TcChildPath
