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
 ) -join "`r`n"
 $taskPreviousVersion = ''
 if (Test-Path -LiteralPath $taskVersionPath) {
  $taskMatch = Select-String -LiteralPath $taskVersionPath -Pattern '^#define TC_MODLOADER_VERSION_STRING "([^"]+)"'
  if ($taskMatch) { $taskPreviousVersion = $taskMatch.Matches[0].Groups[1].Value }
 }
 Set-Content -LiteralPath $taskVersionPath -Value ($taskVersionHeader + "`r`n") -Encoding ascii -NoNewline
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
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\component-model.cpp -o build\component-model-test.exe
 if($LASTEXITCODE){throw 'Component model test build failed'}
 & .\build\component-model-test.exe
 if($LASTEXITCODE){throw 'Component model tests failed'}
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
 # Board handle probe: the unit test above covers the registry in isolation,
 # this pair covers the real level lifetime.  dev.game-handle-probe is the
 # read-only package a player can drop in; the driver build (same source plus
 # tests/game-handle-probe-driver.hpp) enters a board, leaves it and enters
 # another one, because only the game can produce a scene change.
 $taskHandleProbe=Join-Path $taskRoot 'build\game-handle-probe-package'
 New-Item -ItemType Directory -Force (Join-Path $taskHandleProbe 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk tests\game-handle-probe.cpp -o (Join-Path $taskHandleProbe 'native\game-handle-probe.dll')
 if($LASTEXITCODE){throw 'Game handle probe build failed'}
 ('{"format":2,"id":"dev.game-handle-probe","name":"Game handle probe","version":"0.1.0","capabilities":["log","events","game_handles"],"native":{"api":1,"entry":"native/game-handle-probe.dll"}}') | Set-Content (Join-Path $taskHandleProbe 'mod.json') -Encoding ascii
 if(Test-Path dist\dev.game-handle-probe.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.game-handle-probe.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskHandleProbe -Output (Join-Path $taskRoot 'dist\dev.game-handle-probe.mod')
 $taskHandleDriver=Join-Path $taskRoot 'build\game-handle-probe-driver-package'
 New-Item -ItemType Directory -Force (Join-Path $taskHandleDriver 'native') | Out-Null
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared -Isdk -DTC_HANDLE_PROBE_DRIVER=1 tests\game-handle-probe.cpp -o (Join-Path $taskHandleDriver 'native\game-handle-probe.dll')
 if($LASTEXITCODE){throw 'Game handle probe driver build failed'}
 ('{"format":2,"id":"dev.game-handle-probe-driver","name":"Game handle probe driver","version":"0.1.0","capabilities":["log","events","game_handles"],"native":{"api":1,"entry":"native/game-handle-probe.dll"}}') | Set-Content (Join-Path $taskHandleDriver 'mod.json') -Encoding ascii
 if(Test-Path dist\dev.game-handle-probe-driver.mod){Remove-Item -LiteralPath (Join-Path $taskRoot 'dist\dev.game-handle-probe-driver.mod')}
 & .\tools\Pack-Mod.ps1 -Source $taskHandleDriver -Output (Join-Path $taskRoot 'dist\dev.game-handle-probe-driver.mod')
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared tests\component-cost-probe.cpp -o build\component-cost-probe.dll
 if($LASTEXITCODE){throw 'Component cost probe build failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\and-component-fixture.cpp -o build\and-component-fixture.exe
 if($LASTEXITCODE){throw 'AND component fixture build failed'}
 & .\build\and-component-fixture.exe build\and2_component.data
 if($LASTEXITCODE){throw 'AND component fixture generation failed'}
 foreach($taskBoard in @('single','mixed','multi','not1board','and3board','adderboard','double8board','xor8board','mux8board','asr8board','adder8board')) {
  & .\build\and-component-fixture.exe build\and2_component.data 1 1 ('nl-'+$taskBoard)
  if($LASTEXITCODE){throw ('Native logic '+$taskBoard+' board generation failed')}
 }
 foreach($taskShape in @('not1','and3','adder','double8','xor8','mux8','asr8','adder8')) {
  & .\build\and-component-fixture.exe build\and2_component.data 1 1 ('nl-def-'+$taskShape)
  if($LASTEXITCODE){throw ('Native logic definition '+$taskShape+' generation failed')}
 }
 & node tests\and-component-netlist.js build\and2_component.data
 if($LASTEXITCODE){throw 'AND component netlist test failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk tests\and-component-probe.cpp -o build\and-component-probe.dll
 if($LASTEXITCODE){throw 'AND component probe build failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk tests\component-placement-probe.cpp -o build\component-placement-probe.dll
 if($LASTEXITCODE){throw 'Component placement probe build failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk tests\component-persistence-probe.cpp -o build\component-persistence-probe.dll
 if($LASTEXITCODE){throw 'Component persistence probe build failed'}
} finally {
 $env:SOURCE_DATE_EPOCH = $taskPreviousSourceDateEpoch
 Pop-Location
}
