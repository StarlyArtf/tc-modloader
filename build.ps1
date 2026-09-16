$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$taskCompiler = 'C:\msys64\ucrt64\bin'
if ($env:TC_MINGW_BIN) { $taskCompiler = $env:TC_MINGW_BIN }
Push-Location $taskRoot
try {
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
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -Wno-misleading-indentation -Wno-cast-function-type -static -static-libgcc -static-libstdc++ -shared -Ivendor -Ivendor\minhook\include src\loader.cpp src\proxy.def @taskObjects @taskHooks -lbcrypt -lshell32 -o dist\tc-loader.dll
 if ($LASTEXITCODE) { throw 'Loader build failed' }
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
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -municode -Ivendor -Ivendor\minhook\include tests\native-host.cpp @taskObjects @taskHooks -lbcrypt -o build\native-host.exe
 if($LASTEXITCODE){throw 'Native host test build failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -Wno-cast-function-type tests\game-model.cpp -o build\game-model-test.exe
 if($LASTEXITCODE){throw 'Game model test build failed'}
 & .\build\game-model-test.exe
 if($LASTEXITCODE){throw 'Game model tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\component-model.cpp -o build\component-model-test.exe
 if($LASTEXITCODE){throw 'Component model test build failed'}
 & .\build\component-model-test.exe
 if($LASTEXITCODE){throw 'Component model tests failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static tests\and-component-fixture.cpp -o build\and-component-fixture.exe
 if($LASTEXITCODE){throw 'AND component fixture build failed'}
 & .\build\and-component-fixture.exe build\and2_component.data
 if($LASTEXITCODE){throw 'AND component fixture generation failed'}
 & "$taskCompiler\g++.exe" -std=c++17 -O2 -static -shared -Isdk tests\and-component-probe.cpp -o build\and-component-probe.dll
 if($LASTEXITCODE){throw 'AND component probe build failed'}
} finally { Pop-Location }
