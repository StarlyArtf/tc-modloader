$ErrorActionPreference='Stop'
Push-Location $PSScriptRoot
try {
 $taskStage=Join-Path $PSScriptRoot ('build\release-'+[guid]::NewGuid().ToString('N'))
 New-Item -ItemType Directory -Force $taskStage,(Join-Path $taskStage 'licenses'),(Join-Path $taskStage 'tools') | Out-Null
 Compress-Archive -Path src,vendor,sdk,tools,tests,examples,build.ps1,package.ps1,README.md,MOD-FORMAT.md,SDK-GUIDE.md,VALIDATION.md,LICENSE -DestinationPath dist\TCModLoader-0.3.0-source.zip -Force
 Copy-Item dist\TCModLoader-Setup.exe,dist\example.menu-demo.mod,dist\example.cycle-guard.mod,dist\example.circuit-and.mod,dist\TCModLoader-0.3.0-source.zip,README.md,MOD-FORMAT.md,SDK-GUIDE.md,VALIDATION.md,LICENSE $taskStage
 Copy-Item dist\tcmod-cli.exe,tools\Pack-Mod.ps1 (Join-Path $taskStage 'tools')
 Copy-Item sdk $taskStage -Recurse
 Copy-Item vendor\LICENSE (Join-Path $taskStage 'licenses\MINIZ-LICENSE.txt')
 Copy-Item vendor\JSON-LICENSE (Join-Path $taskStage 'licenses\JSON-LICENSE.txt')
 Copy-Item vendor\minhook\LICENSE.txt (Join-Path $taskStage 'licenses\MINHOOK-LICENSE.txt')
 Copy-Item vendor\runtime-licenses\* (Join-Path $taskStage 'licenses')
 $taskHashes=Get-ChildItem $taskStage -Recurse -File | Get-FileHash -Algorithm SHA256 | ForEach-Object { $_.Hash.ToLower()+'  '+$_.Path.Substring($taskStage.Length+1).Replace('\','/') }
 $taskHashes | Set-Content (Join-Path $taskStage 'SHA256SUMS.txt') -Encoding ascii
 Compress-Archive -Path (Join-Path $taskStage '*') -DestinationPath dist\TCModLoader-0.3.0-win64.zip -Force
 Get-Item dist\TCModLoader-0.3.0-win64.zip,dist\TCModLoader-0.3.0-source.zip | Select-Object Name,Length
} finally { Pop-Location }

