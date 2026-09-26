$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$taskCompiler = 'C:\msys64\ucrt64\bin'
if ($env:TC_MINGW_BIN) { $taskCompiler = $env:TC_MINGW_BIN }
$taskPreviousSourceDateEpoch = $env:SOURCE_DATE_EPOCH
# GNU ld embeds the current time in PE files unless SOURCE_DATE_EPOCH is set.
# Use the same default as the deterministic ZIP writer; an official release can
# override it with its publication timestamp before invoking this script.
if (!$env:SOURCE_DATE_EPOCH) { $env:SOURCE_DATE_EPOCH = '315532800' }
Push-Location $taskRoot
try {
 & .\tools\generate-compat.ps1
 if ($LASTEXITCODE) { throw 'Compatibility profile generation failed' }
 # VERSION is the single source of truth for the build number.  The banner, the
 # installer title, the CLI's --version and the package file names all come from
 # the header generated here, so they cannot drift apart the way they used to.
 $taskVersionPath = Join-Path $taskRoot 'src\version.hpp'
 $taskVersion = (Get-Content -LiteralPath (Join-Path $taskRoot 'VERSION') -Raw).Trim()
 if ($taskVersion -notmatch '^(\d+)\.(\d+)\.(\d+)$') { throw "VERSION must be MAJOR.MINOR.PATCH (got '$taskVersion')" }
 $taskMajorNumber = [int]$Matches[1]; $taskMinorNumber = [int]$Matches[2]; $taskPatchNumber = [int]$Matches[3]
 $taskVersionHeader = @(
  '#pragma once',
  '/* Generated from VERSION by build.ps1 - do not edit by hand.',
  '   The build rewrites this file on every run; the checked-in copy is a fallback',
  '   for editors and for a direct g++ invocation.  Edit VERSION instead. */',
  ('#define TC_MODLOADER_VERSION_STRING "{0}"' -f $taskVersion),
  ('#define TC_MODLOADER_VERSION_MAJOR {0}' -f $taskMajorNumber),
  ('#define TC_MODLOADER_VERSION_MINOR {0}' -f $taskMinorNumber),
  ('#define TC_MODLOADER_VERSION_PATCH {0}' -f $taskPatchNumber),
  '#define TC_MODLOADER_VERSION_CODE ((TC_MODLOADER_VERSION_MAJOR<<16)|(TC_MODLOADER_VERSION_MINOR<<8)|TC_MODLOADER_VERSION_PATCH)',
  ('#define TC_MODLOADER_VERSION_WTEXT L"{0}"' -f $taskVersion)
 ) -join "`n"
 $taskPreviousVersion = ''
 if (Test-Path -LiteralPath $taskVersionPath) {
  $taskMatch = Select-String -LiteralPath $taskVersionPath -Pattern '^#define TC_MODLOADER_VERSION_STRING "([^"]+)"'
  if ($taskMatch) { $taskPreviousVersion = $taskMatch.Matches[0].Groups[1].Value }
 }
 # LF, because .gitattributes says `*.hpp text eol=lf`: a CRLF here made every
 # build dirty the working copy, and the release pipeline refuses to package a
 # tree that its own build changed.
 Set-Content -LiteralPath $taskVersionPath -Value ($taskVersionHeader + "`n") -Encoding ascii -NoNewline
 if ($taskPreviousVersion -ne $taskVersion) { Write-Host "Version: $taskPreviousVersion -> $taskVersion (src/version.hpp regenerated)" }
 New-Item -ItemType Directory -Force build,dist | Out-Null
 if (!(Test-Path src\proxy.def)) { & node tools\exports.js; if ($LASTEXITCODE) { throw 'Export generation failed' } }
 foreach ($taskFile in @('miniz','miniz_tdef','miniz_tinfl','miniz_zip')) {
  & "$taskCompiler\gcc.exe" -O2 -Ivendor -c "vendor\$taskFile.c" -o "build\$taskFile.o"
  if ($LASTEXITCODE) { throw 'C compilation failed' }
 }
 $taskObjects = @('build\miniz.o','build\miniz_tdef.o','build\miniz_tinfl.o','build\miniz_zip.o')
 $taskHooks=@()
 foreach($taskHook in @('buffer','hook','trampoline','hde/hde64')) {
  $taskObject='build\mh-'+($taskHook.Replace('/','-'))+'.o'
  & "$taskCompiler\gcc.exe" -O2 -Ivendor\minhook\include -c "vendor\minhook\src\$taskHook.c" -o $taskObject
  if($LASTEXITCODE){throw 'MinHook build failed'}
  $taskHooks+=$taskObject
 }
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Wno-misleading-indentation -Wno-cast-function-type -static -static-libgcc -static-libstdc++ -shared -Ivendor -Ivendor\minhook\include src\loader.cpp src\proxy.def @taskObjects @taskHooks -lbcrypt -lshell32 -lopengl32 -lole32 -lwindowscodecs -o dist\tc-loader.dll
if ($LASTEXITCODE) { throw 'Loader build failed' }
# The same source built as the standalone pin-name patch: the same proxy exports
# (it is still loaded as game_engine.dll) with none of the loader's mod handling.
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Wno-misleading-indentation -Wno-cast-function-type -Wno-unused-function -Wno-unused-variable -DTC_PIN_PATCH_ONLY -static -static-libgcc -static-libstdc++ -shared -Ivendor -Ivendor\minhook\include src\loader.cpp src\proxy.def @taskObjects @taskHooks -lbcrypt -lshell32 -lopengl32 -lole32 -lwindowscodecs -o build\pin-names-patch.dll
if ($LASTEXITCODE) { throw 'Pin name patch build failed' }
New-Item -ItemType Directory -Force (Join-Path $taskRoot 'dist\pin-names-patch') | Out-Null
Copy-Item build\pin-names-patch.dll (Join-Path $taskRoot 'dist\pin-names-patch\game_engine.dll') -Force
 # Mod form of the same patch.  The ordinary loader build above excludes the
 # feature; this variant registers its four hooks through the host and is only
 # active when the player enables local.pin-names-patch.
 $taskPinNamesMod=Join-Path $taskRoot 'build\pin-names-mod-package'
 if(Test-Path -LiteralPath $taskPinNamesMod){Remove-Item -LiteralPath $taskPinNamesMod -Recurse -Force}
 New-Item -ItemType Directory -Force (Join-Path $taskPinNamesMod 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Wno-misleading-indentation -Wno-cast-function-type -Wno-unused-function -Wno-unused-variable -DTC_PIN_PATCH_ONLY -DTC_PIN_PATCH_PLUGIN -static -static-libgcc -static-libstdc++ -shared -Ivendor -Ivendor\minhook\include src\loader.cpp @taskObjects @taskHooks -lbcrypt -lshell32 -lopengl32 -lole32 -lwindowscodecs -o (Join-Path $taskPinNamesMod 'native\pin-names-patch.dll')
 if($LASTEXITCODE){throw 'Pin name patch mod build failed'}
 Copy-Item examples\pin-names-patch\mod.json $taskPinNamesMod -Force
 if(Test-Path dist\local.pin-names-patch.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\local.pin-names-patch.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskPinNamesMod -Output (Join-Path $taskRoot 'dist\local.pin-names-patch.mod')
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -municode -Ivendor src\cli.cpp @taskObjects -lbcrypt -o dist\tcmod-cli.exe
 if ($LASTEXITCODE) { throw 'CLI build failed' }
 '101 RCDATA "../dist/tc-loader.dll"' | Set-Content build\setup.rc -Encoding ascii
 & "$taskCompiler\windres.exe" -Ibuild build\setup.rc -O coff -o build\setup-resource.o
 if ($LASTEXITCODE) { throw 'Resource build failed' }
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -municode -mwindows -Ivendor src\setup.cpp build\setup-resource.o @taskObjects -lbcrypt -lcomdlg32 -lshell32 -o dist\TCModLoader-Setup.exe
 if ($LASTEXITCODE) { throw 'Installer build failed' }
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared examples\cycle-guard\plugin.cpp -o examples\cycle-guard\native\cycle-guard.dll
 if($LASTEXITCODE){throw 'Example plugin build failed'}
 $taskExample=Join-Path $taskRoot 'build\cycle-guard-package'
 New-Item -ItemType Directory -Force (Join-Path $taskExample 'native') | Out-Null
 Copy-Item examples\cycle-guard\mod.json $taskExample -Force
 Copy-Item examples\cycle-guard\native\cycle-guard.dll (Join-Path $taskExample 'native') -Force
 if(Test-Path dist\example.cycle-guard.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\example.cycle-guard.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskExample -Output (Join-Path $taskRoot 'dist\example.cycle-guard.mod')
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk examples\mod-inspector\plugin.cpp -o examples\mod-inspector\native\mod-inspector.dll
 if($LASTEXITCODE){throw 'Inspector example build failed'}
 $taskInspector=Join-Path $taskRoot 'build\mod-inspector-package'
 New-Item -ItemType Directory -Force (Join-Path $taskInspector 'native') | Out-Null
 Copy-Item examples\mod-inspector\mod.json $taskInspector -Force
 Copy-Item examples\mod-inspector\native\mod-inspector.dll (Join-Path $taskInspector 'native') -Force
 if(Test-Path dist\tcmod.mod-inspector.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\tcmod.mod-inspector.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskInspector -Output (Join-Path $taskRoot 'dist\tcmod.mod-inspector.mod')
 # The sandbox simulator (docs/PLAN-sandbox-simulator.md, S2): the loader's own
 # engine drives the board, so the package carries the simcore headers it was
 # built against.
 $taskSandboxSim=Join-Path $taskRoot 'build\sandbox-sim-package'
 New-Item -ItemType Directory -Force (Join-Path $taskSandboxSim 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk examples\sandbox-sim\plugin.cpp -o (Join-Path $taskSandboxSim 'native\sandbox-sim.dll')
 if($LASTEXITCODE){throw 'Sandbox simulator build failed'}
 # The simulator reads the interactive switch/button from their instance
 # configuration (tc.component.instances + tc.component.storage), which is why
 # it needs the "component" capability as well.
 '{"format":2,"id":"dev.sandbox-sim","name":"Sandbox simulator (simcore)","version":"0.1.0","capabilities":["log","symbol","hook","component"],"native":{"api":1,"entry":"native/sandbox-sim.dll"}}' | Set-Content (Join-Path $taskSandboxSim 'mod.json') -Encoding utf8
 if(Test-Path dist\dev.sandbox-sim.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.sandbox-sim.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskSandboxSim -Output (Join-Path $taskRoot 'dist\dev.sandbox-sim.mod')
 if($LASTEXITCODE){throw 'Sandbox simulator package failed'}
 New-Item -ItemType Directory -Force examples\circuit-and\native | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk examples\circuit-and\plugin.cpp -o examples\circuit-and\native\circuit-and.dll
 if($LASTEXITCODE){throw 'Circuit AND example build failed'}
 $taskCircuitAnd=Join-Path $taskRoot 'build\circuit-and-package'
 New-Item -ItemType Directory -Force (Join-Path $taskCircuitAnd 'native') | Out-Null
 Copy-Item examples\circuit-and\mod.json $taskCircuitAnd -Force
 Copy-Item examples\circuit-and\native\circuit-and.dll (Join-Path $taskCircuitAnd 'native') -Force
 if(Test-Path dist\example.circuit-and.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\example.circuit-and.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskCircuitAnd -Output (Join-Path $taskRoot 'dist\example.circuit-and.mod')
 New-Item -ItemType Directory -Force examples\custom-or\native | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk examples\custom-or\plugin.cpp -o examples\custom-or\native\custom-or.dll
 if($LASTEXITCODE){throw 'Custom OR example build failed'}
 $taskCustomOr=Join-Path $taskRoot 'build\custom-or-package'
 New-Item -ItemType Directory -Force (Join-Path $taskCustomOr 'native') | Out-Null
 Copy-Item examples\custom-or\mod.json $taskCustomOr -Force
 Copy-Item examples\custom-or\native\custom-or.dll (Join-Path $taskCustomOr 'native') -Force
 if(Test-Path dist\example.custom-or.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\example.custom-or.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskCustomOr -Output (Join-Path $taskRoot 'dist\example.custom-or.mod')
 # Optional custom drawing SDK example.
 $taskDrawing=Join-Path $taskRoot 'build\drawing-demo-package'
 New-Item -ItemType Directory -Force (Join-Path $taskDrawing 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared examples\drawing-demo\plugin.cpp -o (Join-Path $taskDrawing 'native\drawing-demo.dll')
 if($LASTEXITCODE){throw 'Drawing example build failed'}
 Copy-Item examples\drawing-demo\mod.json $taskDrawing -Force
 Copy-Item examples\drawing-demo\native\images (Join-Path $taskDrawing 'native') -Recurse -Force
 if(Test-Path dist\example.drawing-demo.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\example.drawing-demo.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskDrawing -Output (Join-Path $taskRoot 'dist\example.drawing-demo.mod')
 # Live waveform panel: the level's own inputs and outputs per cycle, drawn in a
 # board side panel, plus a standard VCD export (sdk/tc_trace.h).  The driver
 # build adds tests/waveform-driver.hpp, which enters a level by itself and runs
 # it, so tests/waveform-playtest.ps1 can assert on real traced data.
 $taskWaveform=Join-Path $taskRoot 'build\waveform-demo-package'
 New-Item -ItemType Directory -Force (Join-Path $taskWaveform 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk examples\waveform-demo\plugin.cpp -o (Join-Path $taskWaveform 'native\waveform-demo.dll')
 if($LASTEXITCODE){throw 'Waveform example build failed'}
 Copy-Item examples\waveform-demo\mod.json $taskWaveform -Force
 if(Test-Path dist\example.waveform-demo.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\example.waveform-demo.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskWaveform -Output (Join-Path $taskRoot 'dist\example.waveform-demo.mod')
 $taskWaveformDriver=Join-Path $taskRoot 'build\waveform-driver-package'
 New-Item -ItemType Directory -Force (Join-Path $taskWaveformDriver 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk -DTC_WAVE_DRIVER=1 examples\waveform-demo\plugin.cpp -o (Join-Path $taskWaveformDriver 'native\waveform-demo.dll')
 if($LASTEXITCODE){throw 'Waveform driver build failed'}
 '{"format":2,"id":"dev.waveform-demo-driver","name":"Waveform demo driver","version":"0.1.0","native":{"api":1,"entry":"native/waveform-demo.dll"}}' | Set-Content (Join-Path $taskWaveformDriver 'mod.json') -Encoding ascii
 if(Test-Path dist\dev.waveform-demo-driver.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.waveform-demo-driver.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskWaveformDriver -Output (Join-Path $taskRoot 'dist\dev.waveform-demo-driver.mod')
 # WordWatchee 64: a player-facing fix package.  The plugin raises the
 # renderer's value_size ceiling from 32 to 64 (the shader already implements
 # 33..64 bit hex, unsigned and signed decimal); the manifest carries the
 # one-line exact patch for the shader's own 64-bit hex offset.  Diagnosis and
 # evidence: docs/research/word-watchee-64.md.
 $taskWatchee=Join-Path $taskRoot 'build\word-watchee-64-package'
 # Staged from scratch: the package ships whole shader files, so a leftover copy
 # from an older layout would silently end up in the archive.
 if(Test-Path -LiteralPath $taskWatchee){Remove-Item -LiteralPath $taskWatchee -Recurse -Force}
 New-Item -ItemType Directory -Force (Join-Path $taskWatchee 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk examples\word-watchee-64\plugin.cpp -o (Join-Path $taskWatchee 'native\word-watchee-64.dll')
 if($LASTEXITCODE){throw 'Word watchee 64 mod build failed'}
 Copy-Item examples\word-watchee-64\mod.json $taskWatchee -Force
 Copy-Item examples\word-watchee-64\files $taskWatchee -Recurse -Force
 if(Test-Path dist\local.word-watchee-64.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\local.word-watchee-64.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskWatchee -Output (Join-Path $taskRoot 'dist\local.word-watchee-64.mod')
# Wide constants: append the game's own punch-tape sprite to the native
# component drawer, persist clicks through set_setting, and update the JIT's
# runtime constant slot without recompiling the whole board.
 $taskPunchTape=Join-Path $taskRoot 'build\punch-tape-package'
 if(Test-Path -LiteralPath $taskPunchTape){Remove-Item -LiteralPath $taskPunchTape -Recurse -Force}
 New-Item -ItemType Directory -Force (Join-Path $taskPunchTape 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk examples\punch-tape\plugin.cpp -o (Join-Path $taskPunchTape 'native\punch-tape.dll') -lole32 -lwindowscodecs
 if($LASTEXITCODE){throw 'Punch tape mod build failed'}
 Copy-Item examples\punch-tape\mod.json $taskPunchTape -Force
 if(Test-Path dist\local.punch-tape.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\local.punch-tape.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskPunchTape -Output (Join-Path $taskRoot 'dist\local.punch-tape.mod')
# Pin order: frames, grips and drag-to-reorder for the left IO panel.  The
# order itself lives in the loader (src/pin_order.hpp, TC_SERVICE_PIN_ORDER);
# this Mod is only the handle the player drags.
 $taskPinOrder=Join-Path $taskRoot 'build\pin-order-package'
 if(Test-Path -LiteralPath $taskPinOrder){Remove-Item -LiteralPath $taskPinOrder -Recurse -Force}
 New-Item -ItemType Directory -Force (Join-Path $taskPinOrder 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk examples\pin-order\plugin.cpp -o (Join-Path $taskPinOrder 'native\pin-order.dll')
 if($LASTEXITCODE){throw 'Pin order mod build failed'}
 Copy-Item examples\pin-order\mod.json $taskPinOrder -Force
 if(Test-Path dist\local.pin-order.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\local.pin-order.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskPinOrder -Output (Join-Path $taskRoot 'dist\local.pin-order.mod')
 # The board grid: draws the coordinate grid the game never draws, and carries
 # its switch in the game's own Options page (hook options.general).
 $taskBoardGrid=Join-Path $taskRoot 'build\board-grid-package'
 if(Test-Path -LiteralPath $taskBoardGrid){Remove-Item -LiteralPath $taskBoardGrid -Recurse -Force}
 New-Item -ItemType Directory -Force (Join-Path $taskBoardGrid 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk examples\board-grid\plugin.cpp -o (Join-Path $taskBoardGrid 'native\board-grid.dll')
 if($LASTEXITCODE){throw 'Board grid mod build failed'}
 Copy-Item examples\board-grid\mod.json $taskBoardGrid -Force
 if(Test-Path dist\local.board-grid.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\local.board-grid.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskBoardGrid -Output (Join-Path $taskRoot 'dist\local.board-grid.mod')
  # A Mod-registered pure clock source (0 in, 1 bit out).  The sandbox simulator
  # resolves it by custom id and drives it from the simulator-owned cycle.
  $taskClock=Join-Path $taskRoot 'build\clock-package'
  if(Test-Path -LiteralPath $taskClock){Remove-Item -LiteralPath $taskClock -Recurse -Force}
  New-Item -ItemType Directory -Force (Join-Path $taskClock 'native') | Out-Null
  # -lole32 -lwindowscodecs: the value mark decodes the game's own
  # asset/io_state/io_state.png with WIC, the same decoder the loader uses.
  & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk examples\clock\plugin.cpp -o (Join-Path $taskClock 'native\clock.dll') -lole32 -lwindowscodecs
  if($LASTEXITCODE){throw 'Clock mod build failed'}
  Copy-Item examples\clock\mod.json $taskClock -Force
  if(Test-Path dist\local.clock.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\local.clock.mod')}
  & .\tools\Pack-Mod.ps1 -Source $taskClock -Output (Join-Path $taskRoot 'dist\local.clock.mod')
  # Driver build of the same plugin: tests/clock-period-driver.hpp clicks the
  # clock's corner box and types into the value window it opens, so the playtest
  # can assert the whole path (box -> window -> typed number -> configuration).
  $taskClockDriver=Join-Path $taskRoot 'build\clock-driver-package'
  if(Test-Path -LiteralPath $taskClockDriver){Remove-Item -LiteralPath $taskClockDriver -Recurse -Force}
  New-Item -ItemType Directory -Force (Join-Path $taskClockDriver 'native') | Out-Null
  & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Wno-unused-function -static -shared -Isdk -DTC_CLOCK_DRIVER=1 examples\clock\plugin.cpp -o (Join-Path $taskClockDriver 'native\clock-driver.dll') -lole32 -lwindowscodecs
  if($LASTEXITCODE){throw 'Clock driver build failed'}
  '{"format":2,"id":"dev.clock-driver","name":"Clock value-window driver","version":"0.1.0","capabilities":["log","component","logic","services","symbol_alias"],"native":{"api":1,"entry":"native/clock-driver.dll"}}' | Set-Content -LiteralPath (Join-Path $taskClockDriver 'mod.json') -Encoding ascii
  if(Test-Path dist\dev.clock-driver.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.clock-driver.mod')}
  & .\tools\Pack-Mod.ps1 -Source $taskClockDriver -Output (Join-Path $taskRoot 'dist\dev.clock-driver.mod')
  # The scope: one lane per board wire, sampled per cycle through
 # tc.sim.capture, with channels named by tc.sim.channel.
 $taskScope=Join-Path $taskRoot 'build\scope-package'
 if(Test-Path -LiteralPath $taskScope){Remove-Item -LiteralPath $taskScope -Recurse -Force}
 New-Item -ItemType Directory -Force (Join-Path $taskScope 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk examples\scope\plugin.cpp -o (Join-Path $taskScope 'native\scope.dll')
 if($LASTEXITCODE){throw 'Scope mod build failed'}
 Copy-Item examples\scope\mod.json $taskScope -Force
 if(Test-Path dist\local.scope.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\local.scope.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskScope -Output (Join-Path $taskRoot 'dist\local.scope.mod')
 # Driver for that Mod's drag: it plays the drag with posted mouse messages at
 # the rectangles the Mod logged, then reads the order back from the service.
 $taskPinOrderDriver=Join-Path $taskRoot 'build\pin-order-driver-package'
 if(Test-Path -LiteralPath $taskPinOrderDriver){Remove-Item -LiteralPath $taskPinOrderDriver -Recurse -Force}
 New-Item -ItemType Directory -Force (Join-Path $taskPinOrderDriver 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk tests\pin-order-driver.cpp -o (Join-Path $taskPinOrderDriver 'native\pin-order-driver.dll')
 if($LASTEXITCODE){throw 'Pin order driver build failed'}
 ('{"format":2,"id":"dev.pin-order-driver","name":"Pin order drag driver","version":"0.1.0","capabilities":["log","services"],"native":{"api":1,"entry":"native/pin-order-driver.dll"}}') | Set-Content -LiteralPath (Join-Path $taskPinOrderDriver 'mod.json') -Encoding ascii
 if(Test-Path dist\dev.pin-order-driver.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.pin-order-driver.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskPinOrderDriver -Output (Join-Path $taskRoot 'dist\dev.pin-order-driver.mod')
 # Driver for the per-cycle capture (TC_SERVICE_SIM_CAPTURE): the level runs its
 # own test, and this only arms the capture and reads the ring back, so the rows
 # it reports are cycles rather than frames.  It never navigates anything - the
 # example.byte-adder autotest loads the level.
 $taskScopeDriver=Join-Path $taskRoot 'build\scope-capture-driver-package'
 if(Test-Path -LiteralPath $taskScopeDriver){Remove-Item -LiteralPath $taskScopeDriver -Recurse -Force}
 New-Item -ItemType Directory -Force (Join-Path $taskScopeDriver 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk tests\scope-capture-driver.cpp -o (Join-Path $taskScopeDriver 'native\scope-capture-driver.dll')
 if($LASTEXITCODE){throw 'Scope capture driver build failed'}
 ('{"format":2,"id":"dev.scope-capture-driver","name":"Per-cycle capture driver","version":"0.1.0","capabilities":["log","services"],"native":{"api":1,"entry":"native/scope-capture-driver.dll"}}') | Set-Content -LiteralPath (Join-Path $taskScopeDriver 'mod.json') -Encoding ascii
 if(Test-Path dist\dev.scope-capture-driver.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.scope-capture-driver.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskScopeDriver -Output (Join-Path $taskRoot 'dist\dev.scope-capture-driver.mod')
 # Single-byte adder with declared (1 gate, 1 delay) statistics.
 New-Item -ItemType Directory -Force examples\byte-adder\native | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk examples\byte-adder\plugin.cpp -o examples\byte-adder\native\byte-adder.dll
 if($LASTEXITCODE){throw 'Byte adder example build failed'}
 $taskByteAdder=Join-Path $taskRoot 'build\byte-adder-package'
 New-Item -ItemType Directory -Force (Join-Path $taskByteAdder 'native') | Out-Null
 Copy-Item examples\byte-adder\mod.json $taskByteAdder -Force
 Copy-Item examples\byte-adder\native\byte-adder.dll (Join-Path $taskByteAdder 'native') -Force
 if(Test-Path dist\example.byte-adder.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\example.byte-adder.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskByteAdder -Output (Join-Path $taskRoot 'dist\example.byte-adder.mod')
 # Probe for the two 0-pin component shapes (a source and a sink): it reuses the
 # byte-adder level runner and only replaces the registration, so the log says
 # whether the game imported the scaffolds and whether the callbacks ran.
 $taskPinShape=Join-Path $taskRoot 'build\pin-shape-probe-package'
 if(Test-Path -LiteralPath $taskPinShape){Remove-Item -LiteralPath $taskPinShape -Recurse -Force}
 New-Item -ItemType Directory -Force (Join-Path $taskPinShape 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk tests\pin-shape-probe.cpp -o (Join-Path $taskPinShape 'native\pin-shape-probe.dll')
 if($LASTEXITCODE){throw 'Pin shape probe build failed'}
 ('{"format":2,"id":"dev.pin-shape-probe","name":"Pin shape probe","version":"0.1.0","capabilities":["log","hook","symbol","component","logic"],"native":{"api":1,"entry":"native/pin-shape-probe.dll"}}') | Set-Content -LiteralPath (Join-Path $taskPinShape 'mod.json') -Encoding ascii
 if(Test-Path dist\dev.pin-shape-probe.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.pin-shape-probe.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskPinShape -Output (Join-Path $taskRoot 'dist\dev.pin-shape-probe.mod')
 # Diagnostic observer for gate/delay score investigation (dev only).
 $taskCostWatch=Join-Path $taskRoot 'build\cost-watch-package'
 New-Item -ItemType Directory -Force (Join-Path $taskCostWatch 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk tests\cost-watch.cpp -o (Join-Path $taskCostWatch 'native\cost-watch.dll')
 if($LASTEXITCODE){throw 'Cost watch mod build failed'}
 '{"format":2,"id":"dev.cost-watch","name":"Gate and delay score observer","version":"0.1.0","native":{"api":1,"entry":"native/cost-watch.dll"}}' | Set-Content (Join-Path $taskCostWatch 'mod.json') -Encoding utf8
 if(Test-Path dist\dev.cost-watch.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.cost-watch.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskCostWatch -Output (Join-Path $taskRoot 'dist\dev.cost-watch.mod')
 # Built-in prototype table dump (dev only).
 $taskKindList=Join-Path $taskRoot 'build\kind-list-package'
 New-Item -ItemType Directory -Force (Join-Path $taskKindList 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk tests\kind-list-probe.cpp -o (Join-Path $taskKindList 'native\kind-list.dll')
 if($LASTEXITCODE){throw 'Kind list probe build failed'}
 '{"format":2,"id":"dev.kind-list","name":"Built-in prototype table dump","version":"0.1.0","native":{"api":1,"entry":"native/kind-list.dll"}}' | Set-Content (Join-Path $taskKindList 'mod.json') -Encoding utf8
 if(Test-Path dist\dev.kind-list.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.kind-list.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskKindList -Output (Join-Path $taskRoot 'dist\dev.kind-list.mod')
 # Menu page registration + ID isolation probe (dev only).  The peer package is
 # the same source with a different mod id, so two pages with identical widget
 # labels exist at once.
 # The quoted defines travel in a g++ response file: Windows PowerShell 5.1
 # (the interpreter in the documented `powershell -File build.ps1`) drops the
 # embedded double quotes of a native argument, so `-DTC_DEMO_PAGE_ID="settings"`
 # would arrive as `...=settings` and fail to compile.  g++ parses the response
 # file itself, so the quotes survive both 5.1 and PowerShell 7.
 $taskMenuDefs=Join-Path $taskRoot 'build\menu-demo-defs.rsp'
 $taskMenuDemo=Join-Path $taskRoot 'build\menu-demo-package'
 New-Item -ItemType Directory -Force (Join-Path $taskMenuDemo 'native') | Out-Null
 @('"-DTC_DEMO_PAGE_ID=\"settings\""','"-DTC_DEMO_PAGE_TITLE=\"Menu demo\""') | Set-Content -LiteralPath $taskMenuDefs -Encoding ascii
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk '@build\menu-demo-defs.rsp' examples\menu-demo\plugin.cpp -o (Join-Path $taskMenuDemo 'native\menu-demo.dll')
 if($LASTEXITCODE){throw 'Menu demo mod build failed'}
 Copy-Item examples\menu-demo\mod.json $taskMenuDemo -Force
 if(Test-Path dist\dev.menu-demo.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.menu-demo.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskMenuDemo -Output (Join-Path $taskRoot 'dist\dev.menu-demo.mod')
 $taskMenuDemoPeer=Join-Path $taskRoot 'build\menu-demo-peer-package'
 New-Item -ItemType Directory -Force (Join-Path $taskMenuDemoPeer 'native') | Out-Null
 @('"-DTC_DEMO_PAGE_ID=\"settings\""','"-DTC_DEMO_PAGE_TITLE=\"Menu demo peer\""') | Set-Content -LiteralPath $taskMenuDefs -Encoding ascii
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk '@build\menu-demo-defs.rsp' -DTC_DEMO_PEER=1 examples\menu-demo\plugin.cpp -o (Join-Path $taskMenuDemoPeer 'native\menu-demo-peer.dll')
 if($LASTEXITCODE){throw 'Menu demo peer mod build failed'}
 Copy-Item examples\menu-demo\peer\mod.json $taskMenuDemoPeer -Force
 if(Test-Path dist\dev.menu-demo-peer.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.menu-demo-peer.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskMenuDemoPeer -Output (Join-Path $taskRoot 'dist\dev.menu-demo-peer.mod')
 # Playtest build: the same plugin plus an in-process driver that clicks the
 # loader's page entry, the page's button and the back button, then captures
 # the game's own framebuffer (tests/ui-page-driver.hpp).
 $taskMenuDemoDriver=Join-Path $taskRoot 'build\menu-demo-driver-package'
 New-Item -ItemType Directory -Force (Join-Path $taskMenuDemoDriver 'native') | Out-Null
 @('"-DTC_DEMO_PAGE_ID=\"settings\""','"-DTC_DEMO_PAGE_TITLE=\"Menu demo\""') | Set-Content -LiteralPath $taskMenuDefs -Encoding ascii
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk '@build\menu-demo-defs.rsp' -DTC_DEMO_DRIVER=1 examples\menu-demo\plugin.cpp -o (Join-Path $taskMenuDemoDriver 'native\menu-demo.dll') -lopengl32
 if($LASTEXITCODE){throw 'Menu demo driver build failed'}
 # Its own id: make-ui-sandbox matches packages by the id inside them, so a
 # driver package that still called itself dev.menu-demo could not be applied
 # to a sandbox by file name (`Missing or invalid Mod`).
 '{"format":2,"id":"dev.menu-demo-driver","name":"Menu demo driver","version":"0.1.0","native":{"api":1,"entry":"native/menu-demo.dll"}}' | Set-Content (Join-Path $taskMenuDemoDriver 'mod.json') -Encoding ascii
 if(Test-Path dist\dev.menu-demo-driver.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.menu-demo-driver.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskMenuDemoDriver -Output (Join-Path $taskRoot 'dist\dev.menu-demo-driver.mod')
 # ... and the peer's own driver build.  Two page plugins that both click inside
 # the game are what the isolation case needs: an external process cannot inject
 # a click that the game's UI reacts to (measured - the window never leaves its
 # parked position for it), so the click has to come from a plugin.
 $taskMenuDemoPeerDriver=Join-Path $taskRoot 'build\menu-demo-peer-driver-package'
 New-Item -ItemType Directory -Force (Join-Path $taskMenuDemoPeerDriver 'native') | Out-Null
 @('"-DTC_DEMO_PAGE_ID=\"settings\""','"-DTC_DEMO_PAGE_TITLE=\"Menu demo peer\""') | Set-Content -LiteralPath $taskMenuDefs -Encoding ascii
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk '@build\menu-demo-defs.rsp' -DTC_DEMO_PEER=1 -DTC_DEMO_DRIVER=1 examples\menu-demo\plugin.cpp -o (Join-Path $taskMenuDemoPeerDriver 'native\menu-demo-peer.dll') -lopengl32
 if($LASTEXITCODE){throw 'Menu demo peer driver build failed'}
 '{"format":2,"id":"dev.menu-demo-peer-driver","name":"Menu demo peer driver","version":"0.1.0","native":{"api":1,"entry":"native/menu-demo-peer.dll"}}' | Set-Content (Join-Path $taskMenuDemoPeerDriver 'mod.json') -Encoding ascii
 if(Test-Path dist\dev.menu-demo-peer-driver.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.menu-demo-peer-driver.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskMenuDemoPeerDriver -Output (Join-Path $taskRoot 'dist\dev.menu-demo-peer-driver.mod')
 # Circuit-board side panel example, plus a driver-enabled test build of the
 # same source (tests/ui-board-panel-driver.hpp).
 New-Item -ItemType Directory -Force examples\board-panel\native | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared examples\board-panel\plugin.cpp -o examples\board-panel\native\board-panel.dll
 if($LASTEXITCODE){throw 'Board panel example build failed'}
 $taskBoardPanel=Join-Path $taskRoot 'build\board-panel-package'
 New-Item -ItemType Directory -Force (Join-Path $taskBoardPanel 'native') | Out-Null
 Copy-Item examples\board-panel\mod.json $taskBoardPanel -Force
 Copy-Item examples\board-panel\native\board-panel.dll (Join-Path $taskBoardPanel 'native') -Force
 if(Test-Path dist\example.board-panel.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\example.board-panel.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskBoardPanel -Output (Join-Path $taskRoot 'dist\example.board-panel.mod')
 $taskBoardPanelDriver=Join-Path $taskRoot 'build\board-panel-driver-package'
 New-Item -ItemType Directory -Force (Join-Path $taskBoardPanelDriver 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -DTC_BOARD_DRIVER=1 examples\board-panel\plugin.cpp -o (Join-Path $taskBoardPanelDriver 'native\board-panel.dll')
 if($LASTEXITCODE){throw 'Board panel driver build failed'}
 '{"format":2,"id":"dev.board-panel-driver","name":"Board panel driver","version":"0.1.0","native":{"api":1,"entry":"native/board-panel.dll"}}' | Set-Content (Join-Path $taskBoardPanelDriver 'mod.json') -Encoding ascii
 if(Test-Path dist\dev.board-panel-driver.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.board-panel-driver.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskBoardPanelDriver -Output (Join-Path $taskRoot 'dist\dev.board-panel-driver.mod')
 # Keyboard / character-input probe: a page with an InputText plus a driver that
 # types into it with real window messages (tests/ui-keyboard-*.hpp/.ps1).
 $taskKeyboard=Join-Path $taskRoot 'build\keyboard-package'
 New-Item -ItemType Directory -Force (Join-Path $taskKeyboard 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk -DTC_KEY_DRIVER=1 tests\ui-keyboard-probe.cpp -o (Join-Path $taskKeyboard 'native\keyboard.dll') -limm32
 if($LASTEXITCODE){throw 'Keyboard probe build failed'}
 '{"format":2,"id":"dev.ui-keyboard-probe","name":"Keyboard probe","version":"0.1.0","native":{"api":1,"entry":"native/keyboard.dll"}}' | Set-Content (Join-Path $taskKeyboard 'mod.json') -Encoding ascii
 if(Test-Path dist\dev.ui-keyboard-probe.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.ui-keyboard-probe.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskKeyboard -Output (Join-Path $taskRoot 'dist\dev.ui-keyboard-probe.mod')
 # The same probe without the driver, for a person: no automatic clicks, and a
 # circuit-board panel to type into while the board is live.
 $taskKeyboardManual=Join-Path $taskRoot 'build\keyboard-manual-package'
 New-Item -ItemType Directory -Force (Join-Path $taskKeyboardManual 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk tests\ui-keyboard-probe.cpp -o (Join-Path $taskKeyboardManual 'native\keyboard.dll') -limm32
 if($LASTEXITCODE){throw 'Keyboard manual probe build failed'}
 '{"format":2,"id":"dev.ui-keyboard-manual","name":"Keyboard probe (manual)","version":"0.1.0","native":{"api":1,"entry":"native/keyboard.dll"}}' | Set-Content (Join-Path $taskKeyboardManual 'mod.json') -Encoding ascii
 if(Test-Path dist\dev.ui-keyboard-manual.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.ui-keyboard-manual.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskKeyboardManual -Output (Join-Path $taskRoot 'dist\dev.ui-keyboard-manual.mod')
 # Simulation state mapping probe (route 1, dev only).
 $taskSimState=Join-Path $taskRoot 'build\sim-state-package'
 New-Item -ItemType Directory -Force (Join-Path $taskSimState 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk tests\sim-state-probe.cpp -o (Join-Path $taskSimState 'native\sim-state.dll')
 if($LASTEXITCODE){throw 'Simulation state probe build failed'}
 '{"format":2,"id":"dev.sim-state","name":"Simulation state mapping probe","version":"0.1.0","capabilities":["log","symbol","hook","hook_chain"],"native":{"api":1,"entry":"native/sim-state.dll"}}' | Set-Content (Join-Path $taskSimState 'mod.json') -Encoding utf8
 if(Test-Path dist\dev.sim-state.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.sim-state.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskSimState -Output (Join-Path $taskRoot 'dist\dev.sim-state.mod')
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -municode -Ivendor -Ivendor\minhook\include tests\native-host.cpp @taskObjects @taskHooks -lbcrypt -lopengl32 -lole32 -lwindowscodecs -o build\native-host.exe
 if($LASTEXITCODE){throw 'Native host test build failed'}
 # Hook-chain probes: one source built into several packages that join the
 # loader's sim.do chain at different priorities, swallow the game's call, or
 # try the raw-hook path.  The offline driver (tests/hook-chain.cpp) asserts the
 # ordering, the argument edits, the swallow and the chain-point refusal; the
 # "dup" build is the duplicate-target conflict fixture of tests/native.ps1.
 $taskChainStage=Join-Path $taskRoot 'build\hook-chain-probe'
 New-Item -ItemType Directory -Force $taskChainStage | Out-Null
 $taskChainDefs=Join-Path $taskRoot 'build\hook-chain-defs.rsp'
 $taskChainProbes=@(
  [pscustomobject]@{Dir='a';Id='dev.hook-chain-a';Mode='TC_PROBE_ORDER';Tag='a0';Priority='0';Extra=@('TC_PROBE_TAG_B=\"a10\"','TC_PROBE_PRIORITY_B=10')},
  [pscustomobject]@{Dir='b';Id='dev.hook-chain-b';Mode='TC_PROBE_PEER';Tag='b0';Priority='0';Extra=@()},
  [pscustomobject]@{Dir='skip';Id='dev.hook-chain-skip';Mode='TC_PROBE_SKIP';Tag=$null;Priority=$null;Extra=@()},
  [pscustomobject]@{Dir='raw';Id='dev.hook-chain-raw';Mode='TC_PROBE_RAW';Tag=$null;Priority=$null;Extra=@()},
  [pscustomobject]@{Dir='dup';Id='dev.hook-chain-dup';Mode='TC_PROBE_DUP';Tag=$null;Priority=$null;Extra=@()},
  [pscustomobject]@{Dir='events';Id='dev.hook-chain-events';Mode='TC_PROBE_EVENTS';Tag=$null;Priority=$null;Extra=@()},
  [pscustomobject]@{Dir='cursor';Id='dev.hook-chain-cursor';Mode='TC_PROBE_CURSOR';Tag=$null;Priority=$null;Extra=@()},
  [pscustomobject]@{Dir='crash';Id='dev.hook-chain-crash';Mode='TC_PROBE_CRASH';Tag=$null;Priority=$null;Extra=@()}
 )
 foreach($taskProbe in $taskChainProbes){
  $taskDefines=@('-D'+$taskProbe.Mode)
  if($taskProbe.Tag){
   $taskDefines+='-DTC_PROBE_TAG_A=\"'+$taskProbe.Tag+'\"'
   $taskDefines+='-DTC_PROBE_PRIORITY_A='+$taskProbe.Priority
  }
  foreach($taskDefine in $taskProbe.Extra){$taskDefines+='-D'+$taskDefine}
  # Quotes travel in a response file: Windows PowerShell 5.1 drops them when they
  # are passed as native arguments (same reason as the menu-demo defines).
  ($taskDefines | ForEach-Object {'"'+$_+'"'}) | Set-Content -LiteralPath $taskChainDefs -Encoding ascii
  $taskProbeStage=Join-Path $taskRoot ('build\hook-chain-'+$taskProbe.Dir+'-package')
  New-Item -ItemType Directory -Force (Join-Path $taskProbeStage 'native') | Out-Null
  & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk '@build\hook-chain-defs.rsp' tests\hook-chain-probe.cpp -o (Join-Path $taskProbeStage 'native\hook-chain-probe.dll')
  if($LASTEXITCODE){throw ('Hook chain probe build failed: '+$taskProbe.Dir)}
  ('{"format":2,"id":"'+$taskProbe.Id+'","name":"Hook chain probe '+$taskProbe.Dir+'","version":"0.1.0","capabilities":["log","symbol","hook","symbol_alias","hook_chain","events"],"native":{"api":1,"entry":"native/hook-chain-probe.dll"}}') | Set-Content (Join-Path $taskProbeStage 'mod.json') -Encoding ascii
  $taskProbeMod=Join-Path $taskRoot ('dist\'+$taskProbe.Id+'.mod')
  if(Test-Path $taskProbeMod){Remove-Item -LiteralPath $taskProbeMod}
  & .\tools\Pack-Mod.ps1 -Source $taskProbeStage -Output $taskProbeMod
  Copy-Item (Join-Path $taskProbeStage 'native\hook-chain-probe.dll') (Join-Path $taskChainStage ($taskProbe.Dir+'.dll')) -Force
 }
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -municode -Ivendor -Ivendor\minhook\include tests\hook-chain.cpp @taskObjects @taskHooks -lbcrypt -lopengl32 -lole32 -lwindowscodecs -o build\hook-chain-host.exe
 if($LASTEXITCODE){throw 'Hook chain host test build failed'}
 # Why crash containment is not offered: the VEH + setjmp guard recovers in every
 # isolated shape, but not inside the loader.  This probe keeps that measurement
 # reproducible (the conclusion lives in src/fault_guard.hpp and verification.md).
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared tests\fault-guard-helper.cpp -o build\fault-guard-helper.dll
 if($LASTEXITCODE){throw 'Fault guard helper build failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -Ivendor\minhook\include tests\fault-guard-probe.cpp @taskHooks -o build\fault-guard-probe.exe
 if($LASTEXITCODE){throw 'Fault guard probe build failed'}
 & .\build\fault-guard-probe.exe build\fault-guard-helper.dll
 if($LASTEXITCODE){throw 'Fault guard probe failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -Wno-cast-function-type tests\game-model.cpp -o build\game-model-test.exe
 if($LASTEXITCODE){throw 'Game model test build failed'}
 & .\build\game-model-test.exe
 if($LASTEXITCODE){throw 'Game model tests failed'}
 # The save redirect's decision table: a game build the loader cannot verify must
 # never stop the game from starting (see src/save_boot.hpp).
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\save-boot.cpp -o build\save-boot-test.exe
 if($LASTEXITCODE){throw 'Save redirect decision test build failed'}
 & .\build\save-boot-test.exe
 if($LASTEXITCODE){throw 'Save redirect decision test failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\component-model.cpp -o build\component-model-test.exe
 if($LASTEXITCODE){throw 'Component model test build failed'}
 & .\build\component-model-test.exe
 if($LASTEXITCODE){throw 'Component model tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\preview-label-identity.cpp -o build\preview-label-identity-test.exe
 if($LASTEXITCODE){throw 'Preview label identity test build failed'}
 & .\build\preview-label-identity-test.exe
 if($LASTEXITCODE){throw 'Preview label identity tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\io-state-cache.cpp -o build\io-state-cache-test.exe
 if($LASTEXITCODE){throw 'IO state cache test build failed'}
 & .\build\io-state-cache-test.exe
 if($LASTEXITCODE){throw 'IO state cache tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Wno-misleading-indentation -static tests\native-component.cpp -o build\native-component-test.exe
 if($LASTEXITCODE){throw 'Declarative component test build failed'}
 & .\build\native-component-test.exe
 if($LASTEXITCODE){throw 'Declarative component tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\component-timing.cpp -o build\component-timing-test.exe
 if($LASTEXITCODE){throw 'Component timing test build failed'}
 & .\build\component-timing-test.exe
 if($LASTEXITCODE){throw 'Component timing tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\ui-id.cpp -o build\ui-id-test.exe
 if($LASTEXITCODE){throw 'UI ID test build failed'}
 & .\build\ui-id-test.exe
 if($LASTEXITCODE){throw 'UI ID tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\ui-draw.cpp -o build\ui-draw-test.exe
 if($LASTEXITCODE){throw 'UI drawing test build failed'}
 & .\build\ui-draw-test.exe
 if($LASTEXITCODE){throw 'UI drawing tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\ui-texture.cpp -o build\ui-texture-test.exe
 if($LASTEXITCODE){throw 'UI texture test build failed'}
 & .\build\ui-texture-test.exe
 if($LASTEXITCODE){throw 'UI texture tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\ui-slot.cpp -o build\ui-slot-test.exe
 if($LASTEXITCODE){throw 'UI slot test build failed'}
 & .\build\ui-slot-test.exe
 if($LASTEXITCODE){throw 'UI slot tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\ui-key.cpp -o build\ui-key-test.exe
 if($LASTEXITCODE){throw 'UI key test build failed'}
 & .\build\ui-key-test.exe
 if($LASTEXITCODE){throw 'UI key tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\trace.cpp -o build\trace-test.exe
 if($LASTEXITCODE){throw 'Simulation trace test build failed'}
 & .\build\trace-test.exe
 if($LASTEXITCODE){throw 'Simulation trace tests failed'}
 # Host contract: the version in VERSION, the capability table mod.json is
 # checked against, and the dependency version constraints.
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Wno-misleading-indentation -static -Ivendor tests\capabilities.cpp -lbcrypt -o build\capabilities-test.exe
 if($LASTEXITCODE){throw 'Host contract test build failed'}
 & .\build\capabilities-test.exe
 if($LASTEXITCODE){throw 'Host contract tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\game-handles.cpp -o build\game-handles-test.exe
 if($LASTEXITCODE){throw 'Game handle test build failed'}
 & .\build\game-handles-test.exe
 if($LASTEXITCODE){throw 'Game handle tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\services.cpp -o build\services-test.exe
 if($LASTEXITCODE){throw 'Service discovery test build failed'}
 & .\build\services-test.exe
 if($LASTEXITCODE){throw 'Service discovery tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\board-objects.cpp -o build\board-objects-test.exe
 if($LASTEXITCODE){throw 'Board object test build failed'}
 & .\build\board-objects-test.exe
 if($LASTEXITCODE){throw 'Board object tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\board-pins.cpp -o build\board-pins-test.exe
 if($LASTEXITCODE){throw 'Board pin test build failed'}
 & .\build\board-pins-test.exe
 if($LASTEXITCODE){throw 'Board pin tests failed'}
 # The pin-order store behind TC_SERVICE_PIN_ORDER: a fake panel cache with the
 # game's own record layout, moved by key.
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\pin-order.cpp -o build\pin-order-test.exe
 if($LASTEXITCODE){throw 'Pin order test build failed'}
 & .\build\pin-order-test.exe
 if($LASTEXITCODE){throw 'Pin order test failed'}
 # The per-cycle capture kernel behind TC_SERVICE_SIM_CAPTURE: the ring window,
 # gap and restart bookkeeping, the trigger, and the source injection that puts
 # the tick before every `cycle += 1` in the generated program.
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -Ivendor tests\scope-capture.cpp -o build\scope-capture-test.exe
 if($LASTEXITCODE){throw 'Scope capture test build failed'}
 & .\build\scope-capture-test.exe
 if($LASTEXITCODE){throw 'Scope capture test failed'}
 # The component catalogue behind TC_SERVICE_COMPONENT_REGISTRY: declared names,
 # the compiled shape, refusals and the query table.
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\component-registry.cpp -o build\component-registry-test.exe
 if($LASTEXITCODE){throw 'Component registry test build failed'}
 & .\build\component-registry-test.exe
 if($LASTEXITCODE){throw 'Component registry test failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\component-geometry.cpp -o build\component-geometry-test.exe
 if($LASTEXITCODE){throw 'Component geometry test build failed'}
 & .\build\component-geometry-test.exe
 if($LASTEXITCODE){throw 'Component geometry tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\component-render.cpp -o build\component-render-test.exe
 if($LASTEXITCODE){throw 'Component render test build failed'}
 & .\build\component-render-test.exe
 if($LASTEXITCODE){throw 'Component render tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\simulation.cpp -o build\simulation-test.exe
 if($LASTEXITCODE){throw 'Simulation service test build failed'}
 & .\build\simulation-test.exe
 if($LASTEXITCODE){throw 'Simulation service tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\board-edits.cpp -o build\board-edits-test.exe
 if($LASTEXITCODE){throw 'Board edit test build failed'}
 & .\build\board-edits-test.exe
 if($LASTEXITCODE){throw 'Board edit tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\component-tail.cpp -o build\component-tail-test.exe
 if($LASTEXITCODE){throw 'Component tail test build failed'}
 & .\build\component-tail-test.exe
 if($LASTEXITCODE){throw 'Component tail tests failed'}
 # The clock Mod's adjustable period (examples/clock): the wave each ladder value
 # produces, the state/RESET/REFRESH contract, and the period the face draws.  The
 # plugin is one translation unit, so the test drives its real callbacks.  The
 # plugin keeps one deliberately unused experimental frame handler, hence
 # -Wno-unused-function.
 # -lole32 -lwindowscodecs: the plugin the test includes decodes the game's own
 # asset/io_state/io_state.png with WIC.
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Wno-unused-function -static tests\clock-period.cpp -o build\clock-period-test.exe -lole32 -lwindowscodecs
 if($LASTEXITCODE){throw 'Clock period test build failed'}
 & .\build\clock-period-test.exe
 if($LASTEXITCODE){throw 'Clock period tests failed'}
 # Board handle probe: the unit test above covers the registry in isolation,
 # this pair covers the real level lifetime.  dev.game-handle-probe is the
 # read-only package a player can drop in; the driver build (same source plus
 # tests/game-handle-probe-driver.hpp) enters a board, leaves it and enters
 # another one, because only the game can produce a scene change.
 $taskHandleProbe=Join-Path $taskRoot 'build\game-handle-probe-package'
 New-Item -ItemType Directory -Force (Join-Path $taskHandleProbe 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk tests\game-handle-probe.cpp -o (Join-Path $taskHandleProbe 'native\game-handle-probe.dll')
 if($LASTEXITCODE){throw 'Game handle probe build failed'}
 ('{"format":2,"id":"dev.game-handle-probe","name":"Game handle probe","version":"0.1.0","capabilities":["log","events","game_handles","services"],"native":{"api":1,"entry":"native/game-handle-probe.dll"}}') | Set-Content (Join-Path $taskHandleProbe 'mod.json') -Encoding ascii
 if(Test-Path dist\dev.game-handle-probe.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.game-handle-probe.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskHandleProbe -Output (Join-Path $taskRoot 'dist\dev.game-handle-probe.mod')
 $taskHandleDriver=Join-Path $taskRoot 'build\game-handle-probe-driver-package'
 New-Item -ItemType Directory -Force (Join-Path $taskHandleDriver 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk -DTC_HANDLE_PROBE_DRIVER=1 tests\game-handle-probe.cpp -o (Join-Path $taskHandleDriver 'native\game-handle-probe.dll')
 if($LASTEXITCODE){throw 'Game handle probe driver build failed'}
 ('{"format":2,"id":"dev.game-handle-probe-driver","name":"Game handle probe driver","version":"0.1.0","capabilities":["log","events","game_handles","services"],"native":{"api":1,"entry":"native/game-handle-probe.dll"}}') | Set-Content (Join-Path $taskHandleDriver 'mod.json') -Encoding ascii
 if(Test-Path dist\dev.game-handle-probe-driver.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.game-handle-probe-driver.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskHandleDriver -Output (Join-Path $taskRoot 'dist\dev.game-handle-probe-driver.mod')
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared tests\component-cost-probe.cpp -o build\component-cost-probe.dll
 if($LASTEXITCODE){throw 'Component cost probe build failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\and-component-fixture.cpp -o build\and-component-fixture.exe
 if($LASTEXITCODE){throw 'AND component fixture build failed'}
 & .\build\and-component-fixture.exe build\and2_component.data
 if($LASTEXITCODE){throw 'AND component fixture generation failed'}
 & .\build\and-component-fixture.exe build\and2_component_storage.data 1 1 storage
 if($LASTEXITCODE){throw 'Component storage fixture generation failed'}
 foreach($taskBoard in @('single','mixed','multi','not1board','and3board','adderboard','double8board','xor8board','mux8board','asr8board','adder8board','src0board','sink0board','wide9board')) {
  & .\build\and-component-fixture.exe build\and2_component.data 1 1 ('nl-'+$taskBoard)
  if($LASTEXITCODE){throw ('Native logic '+$taskBoard+' board generation failed')}
 }
 # Same board as not1board, but the instance already carries the schema-6 record
 # a previous release of the storage test component saved.
 & .\build\and-component-fixture.exe build\and2_component.data 1 1 'nl-not1legacy'
 if($LASTEXITCODE){throw 'Legacy component storage fixture generation failed'}
 foreach($taskShape in @('not1','and3','adder','double8','xor8','mux8','asr8','adder8')) {
  & .\build\and-component-fixture.exe build\and2_component.data 1 1 ('nl-def-'+$taskShape)
  if($LASTEXITCODE){throw ('Native logic definition '+$taskShape+' generation failed')}
 }
 & node tests\and-component-netlist.js build\and2_component.data
 if($LASTEXITCODE){throw 'AND component netlist test failed'}
 # The WordWatchee 64 fix, checked against the pinned executable and the game's
 # own shader source instead of a copy of either (tests/word-watchee-model.js).
 & node tests\word-watchee-model.js
 if($LASTEXITCODE){throw 'Word watchee model test failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk tests\and-component-probe.cpp -o build\and-component-probe.dll
 if($LASTEXITCODE){throw 'AND component probe build failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk tests\component-placement-probe.cpp -o build\component-placement-probe.dll
 if($LASTEXITCODE){throw 'Component placement probe build failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk tests\component-persistence-probe.cpp -o build\component-persistence-probe.dll
 if($LASTEXITCODE){throw 'Component persistence probe build failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk tests\component-storage-probe.cpp -o build\component-storage-probe.dll
 if($LASTEXITCODE){throw 'Component storage probe build failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk tests\component-placeholder-probe.cpp -o build\component-placeholder-probe.dll
 if($LASTEXITCODE){throw 'Component placeholder probe build failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk tests\component-undo-probe.cpp -o build\component-undo-probe.dll
 if($LASTEXITCODE){throw 'Component undo probe build failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk tests\component-capacity-probe.cpp -o build\component-capacity-probe.dll
 if($LASTEXITCODE){throw 'Component capacity probe build failed'}
} finally {
 $env:SOURCE_DATE_EPOCH = $taskPreviousSourceDateEpoch
 Pop-Location
}
