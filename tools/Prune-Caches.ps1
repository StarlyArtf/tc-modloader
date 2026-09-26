# Prunes the two caches that grow with every rebuild of a Mod.  Run from the
# source directory:
#   powershell -ExecutionPolicy Bypass -File tools\Prune-Caches.ps1 -DryRun
#   powershell -ExecutionPolicy Bypass -File tools\Prune-Caches.ps1
#
# 1. <game>\tc-modloader-data\plugins\<mod id>\<content hash> is the loader's
#    extraction cache.  It keeps one directory per build of a mod, so rebuilding
#    a mod thirty times leaves thirty copies of its native DLL behind (measured:
#    199 stale directories, 384 MB, one evening of work).  state.json records the
#    hash each mod is *running*, so that one is kept; every other version of a mod
#    whose .mod file is still in mods/ can be re-extracted from that file on
#    demand.  A mod with no .mod file left keeps its whole cache: there the cache
#    may be the only copy of that build.
# 2. <game>\mods\*.mod.bak-* are per-build snapshots of a mod package.  The newest
#    one per mod is kept as a single rollback step; the rest are superseded by the
#    current .mod and by the source tree.
#
# Both targets are validated to sit inside the cache/mods directory they are found
# in, and the script refuses anything else.
param([switch]$DryRun)
$ErrorActionPreference = 'Stop'
$game = Split-Path (Split-Path $PSScriptRoot)
$cacheRoot = Join-Path $game 'tc-modloader-data\plugins'
$modsRoot = Join-Path $game 'mods'
$statePath = Join-Path $game 'tc-modloader-data\state.json'
if(!(Test-Path -LiteralPath $cacheRoot) -or !(Test-Path -LiteralPath $modsRoot)) {
  throw "Not a game directory with loader data: $game"
}

$cacheResolved = (Resolve-Path -LiteralPath $cacheRoot).Path
$modsResolved = (Resolve-Path -LiteralPath $modsRoot).Path
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
$installed = Get-ChildItem -LiteralPath $modsRoot -File -Filter '*.mod' |
  Select-Object -ExpandProperty BaseName
$freed = 0
$removed = 0

function Assert-Inside([string]$path, [string]$root) {
  if(!$path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -or
     $path.Length -le $root.Length) {
    throw "Refusing to delete outside $root : $path"
  }
}

# ---- 1. stale plugin cache versions -----------------------------------------
foreach($plugin in Get-ChildItem -LiteralPath $cacheRoot -Directory) {
  $id = $plugin.Name
  if($installed -notcontains $id) {
    "keep all  {0} (no .mod left to re-extract from)" -f $id
    continue
  }
  $active = $state.native.PSObject.Properties[$id].Value
  $versions = Get-ChildItem -LiteralPath $plugin.FullName -Directory |
    Sort-Object LastWriteTime -Descending
  $keep = $versions | Where-Object { $_.Name -eq $active } | Select-Object -First 1
  if(!$keep) { $keep = $versions | Select-Object -First 1 }
  "prune     {0,-24} keep {1} of {2} version(s)" -f $id, $keep.Name.Substring(0, 8), $versions.Count
  foreach($version in $versions) {
    if($version.Name -eq $keep.Name) { continue }
    $target = (Resolve-Path -LiteralPath $version.FullName).Path
    Assert-Inside $target $cacheResolved
    $bytes = (Get-ChildItem -LiteralPath $target -Recurse -File -ErrorAction SilentlyContinue |
      Measure-Object -Property Length -Sum).Sum
    if($DryRun) { continue }
    Remove-Item -LiteralPath $target -Recurse -Force
    $freed += $bytes
    $removed++
  }
}

# ---- 2. superseded mod package snapshots ------------------------------------
$snapshots = Get-ChildItem -LiteralPath $modsRoot -File -Filter '*.mod.bak-*'
$byMod = $snapshots | Group-Object { $_.Name -replace '\.mod\.bak-.*$', '' }
foreach($group in $byMod) {
  $keep = $group.Group | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  "prune     {0,-24} keep {1} of {2} snapshot(s)" -f $group.Name, $keep.Name, $group.Count
  foreach($snapshot in $group.Group) {
    if($snapshot.Name -eq $keep.Name) { continue }
    Assert-Inside $snapshot.FullName $modsResolved
    if($DryRun) { continue }
    Remove-Item -LiteralPath $snapshot.FullName -Force
    $freed += $snapshot.Length
    $removed++
  }
}

if($DryRun) { 'dry run: nothing removed'; exit 0 }
('removed {0} item(s), freed {1:N1} MB' -f $removed, ($freed / 1MB))
