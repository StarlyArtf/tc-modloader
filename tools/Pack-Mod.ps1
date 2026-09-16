param([Parameter(Mandatory=$true)][string]$Source,[Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$taskSource=(Resolve-Path -LiteralPath $Source).Path
$taskOutput=[IO.Path]::GetFullPath($Output)
if (!(Test-Path -LiteralPath (Join-Path $taskSource 'mod.json'))) { throw 'mod.json must be at the source folder root.' }
if ($taskOutput.StartsWith($taskSource+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Output must be outside the source folder.' }
if (Test-Path -LiteralPath $taskOutput) { throw 'Output already exists. Use another filename.' }
$taskArchive = [System.IO.Compression.ZipFile]::Open($taskOutput,[System.IO.Compression.ZipArchiveMode]::Create)
try {
 foreach ($taskFile in (Get-ChildItem -LiteralPath $taskSource -Recurse -File)) {
  $taskRelative = $taskFile.FullName.Substring($taskSource.Length).TrimStart([IO.Path]::DirectorySeparatorChar)
  $taskEntry = $taskRelative.Replace([IO.Path]::DirectorySeparatorChar,'/')
  [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($taskArchive,$taskFile.FullName,$taskEntry,[System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
 }
} finally {
 $taskArchive.Dispose()
}
Write-Output $taskOutput
