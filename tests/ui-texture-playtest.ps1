param([int]$Seconds=35,[string]$Loader='')
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskCompiler=if($env:TC_MINGW_BIN){$env:TC_MINGW_BIN}else{'C:\msys64\ucrt64\bin'}
$taskPackage=Join-Path $taskRepo 'build\texture-test-package'
$taskMod=Join-Path $taskRepo 'dist\dev.texture-test.mod'
$taskSandbox=Join-Path $taskRepo 'build\texture-test-sandbox'
$taskRoot=Join-Path $taskSandbox 'game'
New-Item -ItemType Directory -Force (Join-Path $taskPackage 'native\images') | Out-Null
& "$taskCompiler\g++.exe" -std=c++17 -O2 -Wall -Wextra -static -shared (Join-Path $PSScriptRoot 'ui-texture-engine.cpp') -lopengl32 -lole32 -lwindowscodecs -o (Join-Path $taskPackage 'native\probe.dll')
if($LASTEXITCODE){throw 'Texture probe build failed'}
'{"format":2,"id":"dev.texture-test","name":"Texture integration test","version":"0.1.0","native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content (Join-Path $taskPackage 'mod.json') -Encoding ascii
'not an image' | Set-Content (Join-Path $taskPackage 'native\broken.png') -Encoding ascii
Add-Type -AssemblyName System.Drawing
$taskImage=New-Object System.Drawing.Bitmap 2,2
try {
    $taskImage.SetPixel(0,0,[Drawing.Color]::FromArgb(255,255,0,0))
    $taskImage.SetPixel(1,0,[Drawing.Color]::FromArgb(128,0,255,0))
    $taskImage.SetPixel(0,1,[Drawing.Color]::FromArgb(255,0,0,255))
    $taskImage.SetPixel(1,1,[Drawing.Color]::FromArgb(0,255,255,0))
    $taskImage.Save((Join-Path $taskPackage 'native\images\色块.png'),[Drawing.Imaging.ImageFormat]::Png)
} finally {$taskImage.Dispose()}
if(Test-Path -LiteralPath $taskMod){Remove-Item -LiteralPath $taskMod}
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output $taskMod
& (Join-Path $PSScriptRoot 'make-ui-sandbox.ps1') -Sandbox $taskSandbox -Mods dev.texture-test
if($Loader){Copy-Item -LiteralPath $Loader -Destination (Join-Path $taskRoot 'game_engine.dll') -Force}
$taskLog=Join-Path $taskRoot 'tc-modloader-data\loader.log'
Remove-Item -LiteralPath $taskLog -ErrorAction SilentlyContinue
$taskPreviousProfile=$env:USERPROFILE
$taskPreviousAppData=$env:APPDATA
$taskProcess=$null
try {
    $env:USERPROFILE=Join-Path $taskSandbox 'home'
    $env:APPDATA=Join-Path $env:USERPROFILE 'AppData\Roaming'
    $taskProcess=Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline=(Get-Date).AddSeconds($Seconds)
    do {
        Start-Sleep -Milliseconds 500
        $taskText=if(Test-Path -LiteralPath $taskLog){Get-Content -LiteralPath $taskLog -Raw}else{''}
        if($taskText -match 'TEXTURE FAIL|TEXTURE PASS frames=180|Native failed' -or $taskProcess.HasExited){break}
    } while((Get-Date)-lt $taskDeadline)
} finally {
    $env:USERPROFILE=$taskPreviousProfile;$env:APPDATA=$taskPreviousAppData
    if($taskProcess -and !$taskProcess.HasExited){[void]$taskProcess.CloseMainWindow();if(!$taskProcess.WaitForExit(3000)){Stop-Process -Id $taskProcess.Id}}
}
$taskText=Get-Content -LiteralPath $taskLog -Raw
if($taskText -match 'TEXTURE FAIL|Native failed|Plugin callback threw' -or $taskText -notmatch 'TEXTURE PASS frames=180'){
    Get-Content -LiteralPath $taskLog -Tail 25
    throw 'Texture integration failed'
}
Get-Content -LiteralPath $taskLog | Select-String 'TEXTURE PASS'
'PASS real GPU image upload/readback, Unicode PNG, alpha/UV, isolation, state restoration and deferred release'
