# M0 reconnaissance: does a board with a **cycle** reach the code generator?
#
# The board is injected the same way the cost playtest injects a solution: as
# the profile's own schematic for the sandbox level.  The board is crafted by
# tools/circuit_format.py (see build/cycle_min_board.data): one NAND whose
# output wire lands back on its own input - the manual's "input depends on its
# own output" case.
#
# The cost probe loads the level, runs 8 cycles and writes
# plugin-data/test.component-cost/result.txt.  This script keeps the sandbox
# (no cleanup) so the generated-source dump and loader.log can be inspected.
param(
  [string]$Board = '',
  [switch]$NoDump,
  # Install tests/cycle-allow-probe.cpp: it suppresses the game's circular
  # dependency annotation, which is the "let it through" experiment (C1).
  [switch]$AllowCycle,
  # Install tests/wire-cut-probe.cpp: it makes one wire end dangle *in the board
  # model*, which is the first half of C2 (cut for compile, re-close later).
  [switch]$Cut,
  # Passed to the wire probe as TC_WIRE_PROBE: 'cut' (default) pushes the end on
  # the NAND input away, 'close' puts a dangling end back onto that pin.
  [string]$WireProbe = '',
  [switch]$ReuseSandbox
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskSandbox = Join-Path $taskRepo 'build\gate-delay-m0'
$taskRoot = Join-Path $taskSandbox 'game'
$taskProfile = Join-Path $taskSandbox 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\test.component-cost'
$taskPackage = Join-Path $taskSandbox 'package'
$taskSchema = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default\schematics\architecture\Default'
$taskBoard = if($Board) { $Board } else { Join-Path $taskRepo 'build\cycle_min_board.data' }
if(!(Test-Path -LiteralPath $taskBoard)) { throw "Missing board: $taskBoard" }

if ((Test-Path -LiteralPath $taskSandbox) -and -not $ReuseSandbox) {
    $taskResolved = (Resolve-Path -LiteralPath $taskSandbox).Path
    $taskBuildRoot = (Resolve-Path -LiteralPath (Join-Path $taskRepo 'build')).Path
    if (-not $taskResolved.StartsWith($taskBuildRoot)) { throw "Refusing to remove a sandbox outside build/: $taskSandbox" }
    Remove-Item -LiteralPath $taskSandbox -Recurse -Force
}
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,$taskSchema,(Join-Path $taskRoot 'mods'),(Join-Path $taskData 'fixtures'),(Join-Path $taskPackage 'native') | Out-Null

foreach($taskDir in @('asset','campaign','translations')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath (Join-Path $taskRepo 'build\and2_component.data') -Destination (Join-Path $taskData 'fixtures\and2_component.data')
Copy-Item -LiteralPath (Join-Path $taskRepo 'build\component-cost-probe.dll') -Destination (Join-Path $taskPackage 'native\probe.dll')
'{"format":2,"id":"test.component-cost","name":"Development component cost probe","version":"0.0.1","native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding utf8
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRoot 'mods\probe.mod') | Out-Null
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\example.byte-adder.mod') -Destination (Join-Path $taskRoot 'mods\example.byte-adder.mod') -Force
$taskApply = @('test.component-cost','example.byte-adder')
$taskCompiler = if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
if ($Cut) {
    $taskCutPackage = Join-Path $taskSandbox 'wire-cut-package'
    New-Item -ItemType Directory -Force (Join-Path $taskCutPackage 'native') | Out-Null
    & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -I (Join-Path $taskRepo 'sdk') (Join-Path $PSScriptRoot 'wire-cut-probe.cpp') -o (Join-Path $taskCutPackage 'native\probe.dll')
    if($LASTEXITCODE){ throw 'wire-cut probe build failed' }
    '{"format":2,"id":"test.wire-cut","name":"Board wire cut probe","version":"0.0.1","native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content -LiteralPath (Join-Path $taskCutPackage 'mod.json') -Encoding utf8
    & (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskCutPackage -Output (Join-Path $taskRoot 'mods\wire-cut.mod') | Out-Null
    $taskApply += 'test.wire-cut'
}
if ($AllowCycle) {
    $taskAllowPackage = Join-Path $taskSandbox 'cycle-allow-package'
    New-Item -ItemType Directory -Force (Join-Path $taskAllowPackage 'native') | Out-Null
    & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -I (Join-Path $taskRepo 'sdk') (Join-Path $PSScriptRoot 'cycle-allow-probe.cpp') -o (Join-Path $taskAllowPackage 'native\probe.dll')
    if($LASTEXITCODE){ throw 'cycle-allow probe build failed' }
    '{"format":2,"id":"test.cycle-allow","name":"Circular dependency suppression probe","version":"0.0.1","native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content -LiteralPath (Join-Path $taskAllowPackage 'mod.json') -Encoding utf8
    & (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskAllowPackage -Output (Join-Path $taskRoot 'mods\cycle-allow.mod') | Out-Null
    $taskApply += 'test.cycle-allow'
}
Set-Content -LiteralPath (Join-Path $taskData 'mode.txt') -Value 'plain' -Encoding ascii
Set-Content -LiteralPath (Join-Path $taskData 'level.txt') -Value 'sandbox' -Encoding ascii
Copy-Item -LiteralPath $taskBoard -Destination (Join-Path $taskSchema 'circuit.data') -Force
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskApply
if($LASTEXITCODE){ throw 'Sandbox apply failed' }

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousDump = $env:TC_MODLOADER_DUMP_SOURCE
$taskPreviousWireProbe = $env:TC_WIRE_PROBE
try {
    $env:USERPROFILE = $taskProfile
    $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
    if(-not $NoDump) { $env:TC_MODLOADER_DUMP_SOURCE = '1' }
    if($WireProbe) { $env:TC_WIRE_PROBE = $WireProbe } else { $env:TC_WIRE_PROBE = $null }
    $taskResult = Join-Path $taskData 'result.txt'
    if(Test-Path -LiteralPath $taskResult) { Remove-Item -LiteralPath $taskResult -Force }
    # The engine's own diagnostics (a compiler error, an unhandled exception)
    # only exist on the process's console; a hidden window hides them too.
    $taskStdout = Join-Path $taskSandbox 'game-stdout.log'
    $taskStderr = Join-Path $taskSandbox 'game-stderr.log'
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput $taskStdout -RedirectStandardError $taskStderr
    $taskDeadline = [DateTime]::UtcNow.AddSeconds(90)
    while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
        Start-Sleep -Milliseconds 250
    }
    if(!(Test-Path -LiteralPath $taskResult)) {
        if(!$taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }
        foreach($taskLog in @($taskStdout,$taskStderr)) {
            if(Test-Path -LiteralPath $taskLog) {
                "==== $([IO.Path]::GetFileName($taskLog)): last 60 lines ===="
                Get-Content -LiteralPath $taskLog -Tail 60
            }
        }
        throw "Probe did not finish; inspect $taskRoot\tc-modloader-data\loader.log"
    }
    if(!$taskProcess.HasExited) {
        [void]$taskProcess.CloseMainWindow()
        if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
    }
    "==== probe result ===="
    [IO.File]::ReadAllText($taskResult)
    "==== sandbox kept at ===="
    $taskRoot
    "board: $taskBoard"
} finally {
    $env:USERPROFILE = $taskPreviousProfile
    $env:APPDATA = $taskPreviousAppData
    $env:TC_MODLOADER_DUMP_SOURCE = $taskPreviousDump
    $env:TC_WIRE_PROBE = $taskPreviousWireProbe
}
