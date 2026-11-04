# Development helper: run the *player's own* frozen board in an isolated copy,
# with the generated source dumped, so the compiled dependency graph can be read
# back offline.  Everything happens in a scratch sandbox; the player's profile
# is only read (copied), never written.
param(
  [string]$Profile = '',
  [string]$Level = 'sandbox',
  [int]$Seconds = 60,
  [string]$Source = '',
  [long]$BenchmarkCycles = 0,
  [string]$Loader = '',
  [string]$VerilogMod = '',
  [string]$Circuit = '',
  [string]$ProbeMod = '',
  [switch]$BenchmarkOnce,
  [switch]$JitDump
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskTest = Join-Path $taskRepo ('build\repro-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
if ($Profile) {
  $taskRealProfile = (Resolve-Path -LiteralPath $Profile -ErrorAction Stop).Path
} else {
  $taskSaveSelection = Join-Path $taskGame 'tc-modloader-data\saves.ini'
  if (!(Test-Path -LiteralPath $taskSaveSelection -PathType Leaf)) {
    throw 'No selected save profile; pass -Profile with the profile directory.'
  }
  $taskSelectedProfile = [regex]::Match((Get-Content -LiteralPath $taskSaveSelection -Raw), '(?m)^profile=([A-Za-z0-9][A-Za-z0-9._-]*)\s*$')
  if (!$taskSelectedProfile.Success) { throw 'Invalid save selection; pass -Profile with the profile directory.' }
  $taskRealProfile = Join-Path $env:APPDATA ('Turing Complete Mods\profiles\' + $taskSelectedProfile.Groups[1].Value)
}
if (!(Test-Path -LiteralPath $taskRealProfile -PathType Container)) {
  throw 'The selected profile directory does not exist; pass -Profile with a valid directory.'
}

New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods') | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
$taskLoader = if ($Loader) { (Resolve-Path -LiteralPath $Loader).Path } else { Join-Path $taskRepo 'dist\tc-loader.dll' }
Copy-Item -LiteralPath $taskLoader -Destination (Join-Path $taskRoot 'game_engine.dll')
$taskVerilogMod = if ($VerilogMod) { (Resolve-Path -LiteralPath $VerilogMod).Path } else { Join-Path $taskRepo 'dist\example.tc-verilog-ram.mod' }
Copy-Item -LiteralPath $taskVerilogMod -Destination (Join-Path $taskRoot 'mods\example.tc-verilog-ram.mod')
$taskProbeMod = if ($ProbeMod) { (Resolve-Path -LiteralPath $ProbeMod).Path } else { Join-Path $taskRepo 'dist\dev.pin-shape-probe.mod' }
Copy-Item -LiteralPath $taskProbeMod -Destination (Join-Path $taskRoot 'mods\dev.pin-shape-probe.mod')

# The player's isolated profile: the save slot holds the board under test.
$taskProfiles = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles'
New-Item -ItemType Directory -Force $taskProfiles | Out-Null
Copy-Item -LiteralPath $taskRealProfile -Destination (Join-Path $taskProfiles 'repro-profile') -Recurse

# The Mod workspace that registers the RV32I type, exactly as the player has it.
$taskWorkspace = Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.tc-verilog-ram'
New-Item -ItemType Directory -Force $taskWorkspace | Out-Null
Copy-Item -LiteralPath (Join-Path $taskGame 'tc-modloader-data\plugin-data\example.tc-verilog-ram\tc-verilog') -Destination $taskWorkspace -Recurse
if($Source) {
  Copy-Item -LiteralPath $Source -Destination (Join-Path $taskWorkspace 'tc-verilog\rv32i_single_cycle.v') -Force
}
New-Item -ItemType Directory -Force (Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.pin-shape-probe') | Out-Null
Set-Content -LiteralPath (Join-Path $taskRoot 'tc-modloader-data\plugin-data\dev.pin-shape-probe\autotest.txt') -Value $Level -Encoding ascii

Set-Content -LiteralPath (Join-Path $taskRoot 'tc-modloader-data\saves.ini') -Encoding ascii -Value @"
[saves]
profile=repro-profile
"@

# `apply` replaces the enabled set, so both IDs go in one call.
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply example.tc-verilog-ram dev.pin-shape-probe | Out-Null
if($LASTEXITCODE) { throw 'mod apply failed' }

# Use the schematic kind from level metadata (sandbox is architecture), not
# the level name. An explicit circuit replaces only copies in this sandbox.
$taskSave = if ($Circuit) { (Resolve-Path -LiteralPath $Circuit).Path } else { Join-Path $taskRealProfile 'schematics\architecture\2\circuit.data' }
if(Test-Path -LiteralPath $taskSave) {
  $taskKind = $Level
  $taskMeta = Join-Path $taskRoot ('campaign\' + $Level + '\meta.txt')
  if (Test-Path -LiteralPath $taskMeta) {
    $taskKindMatch = [regex]::Match((Get-Content -LiteralPath $taskMeta -Raw), '(?m)^kind\s*=\s*([A-Za-z0-9_-]+)\s*$')
    if ($taskKindMatch.Success) { $taskKind = $taskKindMatch.Groups[1].Value }
  }
  $taskCategory = Join-Path $taskProfiles ('repro-profile\schematics\' + $taskKind)
  $taskSlot = Join-Path $taskCategory 'Default'
  New-Item -ItemType Directory -Force $taskSlot | Out-Null
  Copy-Item -LiteralPath $taskSave -Destination (Join-Path $taskSlot 'circuit.data') -Force
  if ($Circuit) {
    foreach ($taskCopy in (Get-ChildItem -LiteralPath $taskCategory -Recurse -File -Filter circuit.data)) {
      Copy-Item -LiteralPath $taskSave -Destination $taskCopy.FullName -Force
    }
  }
}

$taskOldProfile = $env:USERPROFILE
$taskOldAppData = $env:APPDATA
$taskOldDump = $env:TC_MODLOADER_DUMP_SOURCE
$taskOldBenchmark = $env:TC_PIN_SHAPE_BENCHMARK_CYCLES
$taskOldOnce = $env:TC_PIN_SHAPE_BENCHMARK_ONCE
$taskOldJitDump = $env:TC_TEST_JIT_DUMP
$taskProcess = $null
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $env:TC_MODLOADER_DUMP_SOURCE = '1'
  $env:TC_PIN_SHAPE_BENCHMARK_ONCE = if ($BenchmarkOnce) { '1' } else { '0' }
  $env:TC_TEST_JIT_DUMP = if ($JitDump) { '1' } else { '0' }
  if($BenchmarkCycles -gt 0) {
    $env:TC_PIN_SHAPE_BENCHMARK_CYCLES = [string]$BenchmarkCycles
  } else {
    Remove-Item Env:TC_PIN_SHAPE_BENCHMARK_CYCLES -ErrorAction SilentlyContinue
  }
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds $Seconds
} finally {
  try {
    if($taskProcess -and !$taskProcess.HasExited) {
      [void]$taskProcess.CloseMainWindow()
      if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
    }
  } finally {
    $env:USERPROFILE = $taskOldProfile
    $env:APPDATA = $taskOldAppData
    $env:TC_MODLOADER_DUMP_SOURCE = $taskOldDump
    $env:TC_PIN_SHAPE_BENCHMARK_CYCLES = $taskOldBenchmark
    $env:TC_PIN_SHAPE_BENCHMARK_ONCE = $taskOldOnce
    $env:TC_TEST_JIT_DUMP = $taskOldJitDump
  }
}

"Sandbox: $taskTest"
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
if(Test-Path -LiteralPath $taskLog) {
  Select-String -LiteralPath $taskLog -Pattern 'Level loaded|board layout|registered custom 0x2927|bound instance|internal circuit' |
    Select-Object -Last 6 | ForEach-Object { $_.Line }
}
Get-ChildItem -Path $taskRoot -Filter 'native-logic-source*.txt' | Select-Object Name,Length
