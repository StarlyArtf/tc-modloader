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

# ---- the installer's own payload -------------------------------------------
#
# The installer is the one file a player runs: it carries the loader it deploys
# as resource 101 (RCDATA) and the version as a Windows version resource, and
# both come out of build.ps1 in order - the loader first, then the version
# block, then the installer linked around that loader.  Nothing used to read the
# pair back, so a release could stage an installer that deploys something else:
# `-UseExistingBuild` skips the build entirely, and a dist/ whose
# tc-loader.dll was replaced by a hand-built DLL (the trap
# docs/HANDOFF-punch-tape-runtime-constants.md describes) leaves an installer
# embedding the older one.  Both halves are cheap to read out of the PE file, so
# a release (and the fast-tier release contract) refuses a mismatched pair.
$script:TcEmbeddedResourceReaderReady = $false

function Initialize-TcEmbeddedResourceReader {
  if ($script:TcEmbeddedResourceReaderReady) { return }
  if (-not ('TcReleaseInstallerReader' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class TcReleaseInstallerReader
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr LoadResource(IntPtr module, IntPtr info);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint SizeofResource(IntPtr module, IntPtr info);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr LockResource(IntPtr data);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FreeLibrary(IntPtr module);

    /* LOAD_LIBRARY_AS_DATAFILE: read the resources of a file that is not - and
       must not become - a loaded module. */
    public static byte[] Read(string path, int name, int type)
    {
        IntPtr module = LoadLibraryExW(path, IntPtr.Zero, 0x2);
        if (module == IntPtr.Zero)
            throw new Exception("could not open " + path + " (Win32 " + Marshal.GetLastWin32Error() + ")");
        try
        {
            IntPtr info = FindResourceW(module, new IntPtr(name), new IntPtr(type));
            if (info == IntPtr.Zero)
                throw new Exception("resource " + name + " of type " + type + " is missing from " + path);
            uint size = SizeofResource(module, info);
            IntPtr data = LockResource(LoadResource(module, info));
            if (data == IntPtr.Zero || size == 0)
                throw new Exception("resource " + name + " of " + path + " is empty");
            byte[] bytes = new byte[size];
            Marshal.Copy(data, bytes, 0, (int)size);
            return bytes;
        }
        finally { FreeLibrary(module); }
    }
}
'@
  }
  $script:TcEmbeddedResourceReaderReady = $true
}

function Get-TcSha256 {
  param([Parameter(Mandatory = $true)][byte[]]$Bytes)
  $taskSha = [Security.Cryptography.SHA256]::Create()
  try { return (($taskSha.ComputeHash($Bytes) | ForEach-Object { $_.ToString('x2') }) -join '') }
  finally { $taskSha.Dispose() }
}

function Get-TcInstallerPayload {
  param([Parameter(Mandatory = $true)][string]$Installer)
  Initialize-TcEmbeddedResourceReader
  # 101 RCDATA is the loader the installer deploys (src/setup_resources.rc).
  return [TcReleaseInstallerReader]::Read($Installer, 101, 10)
}

function Assert-TcInstallerMatchesRelease {
  param([Parameter(Mandatory = $true)][string]$Repo)
  $taskVersion = (Get-Content -LiteralPath (Join-Path $Repo 'VERSION') -Raw).Trim()
  $taskLoader = Join-Path $Repo 'dist\tc-loader.dll'
  $taskInstaller = Join-Path $Repo 'dist\TCModLoader-Setup.exe'
  foreach ($taskPath in @($taskLoader, $taskInstaller)) {
    if (!(Test-Path -LiteralPath $taskPath)) { throw "Release artifact is missing: $taskPath" }
  }
  $taskExpected = Get-TcSha256 ([IO.File]::ReadAllBytes($taskLoader))
  $taskEmbedded = Get-TcSha256 (Get-TcInstallerPayload -Installer $taskInstaller)
  if ($taskEmbedded -cne $taskExpected) {
    throw ("The installer does not deploy the loader this release ships: " +
           "dist\TCModLoader-Setup.exe embeds $taskEmbedded, dist\tc-loader.dll is $taskExpected. " +
           "Run build.ps1 - the installer is linked around the loader it embeds.")
  }
  $taskInfo = (Get-Item -LiteralPath $taskInstaller).VersionInfo
  foreach ($taskField in @('FileVersion', 'ProductVersion')) {
    $taskValue = [string]$taskInfo.$taskField
    if ($taskValue -cne $taskVersion) {
      throw ("The installer's $taskField is '$taskValue' but VERSION says '$taskVersion'. " +
             "Run build.ps1: the version block comes from build\setup-version.rc.")
    }
  }
  return [pscustomobject]@{
    Version = $taskVersion
    LoaderSha256 = $taskExpected
    InstallerSha256 = (Get-FileHash -LiteralPath $taskInstaller -Algorithm SHA256).Hash.ToLowerInvariant()
  }
}

Export-ModuleMember -Function Get-TcArchiveTimestamp, New-TcDeterministicZip, Read-TcModArchive,
  Test-TcChildPath, Get-TcInstallerPayload, Assert-TcInstallerMatchesRelease
