# M2 evidence on the real machine: does a ring behave like a ring under the
# gate-delay mode?
#
# The injected board is the *cut* ring (tools/gate-delay-boards.py): three NANDs
# wired A -> B -> C with the C -> A wire missing, and a placeholder constant
# driving that input, so the graph the compiler sees is acyclic and every
# component is emitted.  The loader re-closes the connection from
# TC_GATE_DELAY_CLOSE, which turns the board back into a ring - and a ring only
# behaves like one under the delay model.
#
# The probe is the component-cost probe with its per-cycle sampling switched on
# (TC_GATE_DELAY_SAMPLE=1): it steps the simulation one cycle at a time and
# records the low state slots, so the assertion is a difference rather than a
# fixed phase -
#
#   TC_GATE_DELAY=1 + re-close   the ring moves, several slots change per cycle
#   mode off                     the same board is acyclic and constant driven:
#                                nothing ever changes
param(
  [string]$Board = '',
  [string]$Close = '2001:2004:2003',
  [int]$Cycles = 12,
  [int]$Slots = 512,
  # The level whose saved schematic the board is injected into.  `architecture`
  # is the sandbox's kind; anything else is a campaign level, which is how the
  # "the mode never touches a normal level" rule is checked.
  [string]$SchemaKind = 'architecture',
  # '' leaves TC_GATE_DELAY alone, 'on'/'off' writes the saved setting the
  # Options page checkbox keeps (tc-modloader-data/gate-delay.txt).
  [string]$SavedSetting = '',
  # Name under build/ for the isolated copy of the game.  A different one is
  # useful when a previous run's process is still holding the files.
  [string]$Sandbox = 'gate-delay-ring',
  # Experiment: replace the injected schematic with another board a while after
  # the game has started but before it compiles.  Tells "the compile reads the
  # file" from "the compile reads a snapshot taken when the level was loaded".
  [string]$SwapBoard = '',
  [int]$SwapAfterMs = 1500
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskSandbox = Join-Path $taskRepo "build\$Sandbox"
$taskRoot = Join-Path $taskSandbox 'game'
$taskProfile = Join-Path $taskSandbox 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\test.component-cost'
$taskPackage = Join-Path $taskSandbox 'package'
$taskSchema = Join-Path $taskProfile ("AppData\Roaming\Turing Complete Mods\profiles\default\schematics\$SchemaKind\Default")
$taskBoard = if($Board) { $Board } else { Join-Path $taskRepo 'build\gate_delay_ring_board.data' }
if(!(Test-Path -LiteralPath $taskBoard)) { throw "Missing board: $taskBoard (run tools/gate-delay-boards.py)" }
$taskCompiler = if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}

if (Test-Path -LiteralPath $taskSandbox) {
    $taskResolved = (Resolve-Path -LiteralPath $taskSandbox).Path
    $taskBuildRoot = (Resolve-Path -LiteralPath (Join-Path $taskRepo 'build')).Path
    if (-not $taskResolved.StartsWith($taskBuildRoot)) { throw "Refusing to remove a sandbox outside build/: $taskSandbox" }
    Remove-Item -LiteralPath $taskSandbox -Recurse -Force
}
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,$taskSchema,(Join-Path $taskRoot 'mods'),$taskData,(Join-Path $taskData 'fixtures'),(Join-Path $taskPackage 'native') | Out-Null

foreach($taskDir in @('asset','campaign','translations')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')

Copy-Item -LiteralPath (Join-Path $taskRepo 'build\and2_component.data') -Destination (Join-Path $taskData 'fixtures\and2_component.data')
# Built here rather than reused: this case needs the probe's per-cycle sampling,
# which is newer than whatever build.ps1 last produced.
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -I (Join-Path $taskRepo 'sdk') `
  (Join-Path $PSScriptRoot 'component-cost-probe.cpp') -o (Join-Path $taskPackage 'native\probe.dll')
if($LASTEXITCODE){ throw 'cost probe build failed' }
'{"format":2,"id":"test.component-cost","name":"Development component cost probe","version":"0.0.1","native":{"api":1,"entry":"native/probe.dll"}}' |
    Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding utf8
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRoot 'mods\probe.mod') | Out-Null
# The board only comes up in a session where the game really entered a level;
# the byte-adder example is what the other playtests load for that.
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\example.byte-adder.mod') -Destination (Join-Path $taskRoot 'mods\example.byte-adder.mod') -Force
# The clock source (examples/clock) is Mod-registered, so the boards that carry
# one need it in the sandbox.  Keeping it enabled for every run is harmless: a
# board without a clock instance simply never mentions it.
$taskEnable = @('test.component-cost','example.byte-adder')
if(Test-Path -LiteralPath (Join-Path $taskRepo 'dist\local.clock.mod')) {
    Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\local.clock.mod') -Destination (Join-Path $taskRoot 'mods\local.clock.mod') -Force
    $taskEnable += 'local.clock'
}

Set-Content -LiteralPath (Join-Path $taskData 'mode.txt') -Value 'plain' -Encoding ascii
Set-Content -LiteralPath (Join-Path $taskData 'level.txt') -Value 'sandbox' -Encoding ascii
Set-Content -LiteralPath (Join-Path $taskData 'sample_cycles.txt') -Value $Cycles -Encoding ascii
Set-Content -LiteralPath (Join-Path $taskData 'sample_slots.txt') -Value $Slots -Encoding ascii
Copy-Item -LiteralPath $taskBoard -Destination (Join-Path $taskSchema 'circuit.data') -Force
if($SavedSetting) {
    $taskLoaderData = Join-Path $taskRoot 'tc-modloader-data'
    New-Item -ItemType Directory -Force $taskLoaderData | Out-Null
    $taskSettingText = if($SavedSetting -eq 'on') { 'on=1' } else { 'on=0' }
    Set-Content -LiteralPath (Join-Path $taskLoaderData 'gate-delay.txt') -Value $taskSettingText -Encoding ascii
}
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply @taskEnable
if($LASTEXITCODE){ throw 'Sandbox apply failed' }

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousDump = $env:TC_MODLOADER_DUMP_SOURCE
$taskPreviousSample = $env:TC_GATE_DELAY_SAMPLE
try {
    $env:USERPROFILE = $taskProfile
    $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
    $env:TC_MODLOADER_DUMP_SOURCE = '1'
    $env:TC_GATE_DELAY_SAMPLE = '1'
    if($Close) { $env:TC_GATE_DELAY_CLOSE = $Close } else { $env:TC_GATE_DELAY_CLOSE = $null }
    $taskResult = Join-Path $taskData 'result.txt'
    if(Test-Path -LiteralPath $taskResult) { Remove-Item -LiteralPath $taskResult -Force }
    $taskStdout = Join-Path $taskSandbox 'game-stdout.log'
    $taskStderr = Join-Path $taskSandbox 'game-stderr.log'
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput $taskStdout -RedirectStandardError $taskStderr
    if($SwapBoard) {
        $taskSwap = Join-Path $taskRepo $SwapBoard
        if(!(Test-Path -LiteralPath $taskSwap)) { throw "Missing swap board: $taskSwap" }
        Start-Sleep -Milliseconds $SwapAfterMs
        Copy-Item -LiteralPath $taskSwap -Destination (Join-Path $taskSchema 'circuit.data') -Force
        "swapped the schematic after $SwapAfterMs ms"
    }
    $taskDeadline = [DateTime]::UtcNow.AddSeconds(150)
    while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
        Start-Sleep -Milliseconds 250
    }
    if(!(Test-Path -LiteralPath $taskResult)) {
        if(!$taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }
        foreach($taskLog in @($taskStdout,$taskStderr)) {
            if(Test-Path -LiteralPath $taskLog) {
                "==== $([IO.Path]::GetFileName($taskLog)): last 30 lines ===="
                Get-Content -LiteralPath $taskLog -Tail 30
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
    "==== loader log (gate delay) ===="
    $taskLoaderLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
    if(Test-Path -LiteralPath $taskLoaderLog) {
        Select-String -LiteralPath $taskLoaderLog -Pattern 'Gate delay' | ForEach-Object { $_.Line } | Select-Object -First 6
    }
    "sandbox: $taskRoot"
} finally {
    $env:USERPROFILE = $taskPreviousProfile
    $env:APPDATA = $taskPreviousAppData
    $env:TC_MODLOADER_DUMP_SOURCE = $taskPreviousDump
    $env:TC_GATE_DELAY_SAMPLE = $taskPreviousSample
    $env:TC_GATE_DELAY_CLOSE = $null
}
