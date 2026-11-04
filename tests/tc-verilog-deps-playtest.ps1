# Real-machine check for the per-output dependency map.
#
# The single-cycle RV32I front end is the case that produced a *false*
# combinational loop: with the built-in register file, read-data -> core ->
# read-address -> register file -> read-data looked like a cycle because the
# generated wrapper made every output depend on every input.  The TC-Verilog mod
# now publishes one input mask per output and the loader builds a private cone
# for each, so the three address outputs depend on `instr` alone.
#
# This launches the real game in a sandbox, seeds the Mod player workspace with
# the RV32I source, and asserts the loader received the exact map (the same table
# tools/verilog-dependencies.cpp prints offline).  If a future change widens an
# address mask back to all inputs, the cycle returns - and this fails.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskTest = Join-Path $taskRepo ('build\tc-verilog-deps-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'

New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods') | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\example.tc-verilog-ram.mod') -Destination (Join-Path $taskRoot 'mods')
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply example.tc-verilog-ram
if($LASTEXITCODE) { throw 'TC-Verilog example package apply failed' }

# Seed the workspace the way a player would: the Mod materialises the package
# files only when the directory is absent, so an existing workspace is read
# verbatim.  A one-component format 1 manifest keeps this run minimal.
$taskWorkspace = Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.tc-verilog-ram\tc-verilog'
New-Item -ItemType Directory -Force $taskWorkspace | Out-Null
Copy-Item -LiteralPath (Join-Path $taskRepo 'tests\fixtures\rv32i_single_cycle.v') -Destination (Join-Path $taskWorkspace 'rv32i_single_cycle.v')
@'
{
  "format": 1,
  "components": [
    {
      "source": "rv32i_single_cycle.v",
      "custom_id": "0x2927d7c021863f27",
      "type_id": "example.tc-verilog-ram/rv32i_single_cycle",
      "name": "rv32i_single_cycle",
      "delay": 1
    }
  ]
}
'@ | Set-Content -LiteralPath (Join-Path $taskWorkspace 'tc-verilog.json') -Encoding ascii

$taskOldProfile = $env:USERPROFILE
$taskOldAppData = $env:APPDATA
function Start-TestGame {
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds 35
  if(!$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
}

try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  Start-TestGame

  $taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
  if(!(Test-Path -LiteralPath $taskLog)) { throw "no loader log; inspect $taskTest" }
  $taskText = Get-Content -LiteralPath $taskLog -Raw
  # pc only for imem_addr; instr only for the three register addresses; all five
  # inputs for the load write-back; instr+pc+rs1 for the data address.
  $taskExpected = 'registered custom 0x2927d7c021863f27 inputs=5 outputs=17 [^\r\n]*deps=0x1,0x2,0x2,0x2,0xf,0x1f,0x7,0xf,0xf,0xf,0xf,0xf,0xf,0xf,0xf,0xf,0xf'
  if($taskText -notmatch $taskExpected) {
    $taskSeen = [regex]::Matches($taskText, 'registered custom 0x2927d7c021863f27[^\r\n]*')
    $taskDetail = ($taskSeen | ForEach-Object { $_.Value }) -join "`n"
    throw "the loader did not receive the RV32I dependency map; saw: $taskDetail ; inspect $taskTest"
  }
} finally {
  $env:USERPROFILE = $taskOldProfile
  $env:APPDATA = $taskOldAppData
}

"PASS TC-Verilog dependencies: the RV32I front end registered with instr-only register addresses"
"Sandbox: $taskTest"
