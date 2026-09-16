$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskGame=Split-Path $taskRepo
$taskFixture=Join-Path $taskRepo ('build\installer-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskFixture | Out-Null
Copy-Item (Join-Path $taskGame 'Turing Complete.exe') $taskFixture
$taskOriginal=Join-Path $taskGame 'tc_game_engine.dll'
if (!(Test-Path $taskOriginal)) { $taskOriginal=Join-Path $taskGame 'game_engine.dll' }
Copy-Item $taskOriginal (Join-Path $taskFixture 'game_engine.dll')
$taskHash=(Get-FileHash (Join-Path $taskFixture 'game_engine.dll')).Hash
$taskSetup=Join-Path $taskRepo 'dist\TCModLoader-Setup.exe'
$taskPass=0
function Invoke-SetupTest([string]$Mode,[int]$Expected) {
 $taskP=Start-Process -FilePath $taskSetup -ArgumentList @($Mode,('"'+$taskFixture+'"')) -WindowStyle Hidden -PassThru -Wait
 if ($taskP.ExitCode -ne $Expected) { throw "Unexpected setup exit: $($taskP.ExitCode), expected $Expected" }
}
Invoke-SetupTest '--install' 0
if ((Get-FileHash (Join-Path $taskFixture 'tc_game_engine.dll')).Hash -ne $taskHash) {throw 'Backup mismatch'}
$taskPass++; Write-Output 'PASS install and byte-identical engine backup'
Invoke-SetupTest '--install' 0
$taskPass++; Write-Output 'PASS repeated install'
Invoke-SetupTest '--uninstall' 0
if ((Get-FileHash (Join-Path $taskFixture 'game_engine.dll')).Hash -ne $taskHash) {throw 'Restore mismatch'}
if (Test-Path (Join-Path $taskFixture 'tc_game_engine.dll')) {throw 'Backup alias should be removed'}
$taskPass++; Write-Output 'PASS uninstall restores exact engine'
Invoke-SetupTest '--uninstall' 0
$taskPass++; Write-Output 'PASS repeated uninstall'
[IO.File]::AppendAllText((Join-Path $taskFixture 'Turing Complete.exe'),'unsupported')
Invoke-SetupTest '--install' 1
if ((Get-FileHash (Join-Path $taskFixture 'game_engine.dll')).Hash -ne $taskHash) {throw 'Unsupported build was changed'}
$taskPass++; Write-Output 'PASS unsupported EXE rejected without modifying engine'
Copy-Item (Join-Path $taskGame 'Turing Complete.exe') $taskFixture -Force
[IO.File]::AppendAllText((Join-Path $taskFixture 'game_engine.dll'),'unsupported')
$taskChanged=(Get-FileHash (Join-Path $taskFixture 'game_engine.dll')).Hash
Invoke-SetupTest '--install' 1
if ((Get-FileHash (Join-Path $taskFixture 'game_engine.dll')).Hash -ne $taskChanged) {throw 'Unknown engine was changed'}
$taskPass++; Write-Output 'PASS unsupported engine rejected without overwrite'
"$taskPass installer tests passed" | Set-Content (Join-Path $taskRepo 'build\installer-test-results.txt')
