$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskGame=Split-Path $taskRepo
Import-Module (Join-Path $taskRepo 'tools\ReleaseTools.psm1') -Force
$taskFixture=Join-Path $taskRepo ('build\installer-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskFixture | Out-Null
Copy-Item (Join-Path $taskGame 'Turing Complete.exe') $taskFixture
$taskOriginal=Join-Path $taskGame 'tc_game_engine.dll'
if (!(Test-Path $taskOriginal)) { $taskOriginal=Join-Path $taskGame 'game_engine.dll' }
Copy-Item $taskOriginal (Join-Path $taskFixture 'game_engine.dll')
$taskHash=(Get-FileHash (Join-Path $taskFixture 'game_engine.dll')).Hash
$taskSetup=Join-Path $taskRepo 'dist\TCModLoader-Setup.exe'
$taskPass=0
function Start-HeadlessSetup([string]$Mode,[string]$Directory) {
 # Avoid ShellExecute's console host under the captured Windows PowerShell
 # runner: it can outlive the installer and hold the test's output pipes open.
 $taskInfo = New-Object Diagnostics.ProcessStartInfo
 $taskInfo.FileName = $taskSetup
 $taskInfo.Arguments = $Mode + ' "' + $Directory + '"'
 $taskInfo.UseShellExecute = $false
 $taskInfo.CreateNoWindow = $true
 $taskP = [Diagnostics.Process]::Start($taskInfo)
 if (!$taskP.WaitForExit(30000)) { $taskP.Kill(); throw 'Installer did not exit within 30 seconds' }
 return $taskP
}
function Invoke-SetupTest([string]$Mode,[int]$Expected) {
 $taskP=Start-HeadlessSetup $Mode $taskFixture
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
Copy-Item $taskOriginal (Join-Path $taskFixture 'tc_game_engine.dll') -Force
Invoke-SetupTest '--uninstall' 1
if ((Get-FileHash (Join-Path $taskFixture 'game_engine.dll')).Hash -ne $taskChanged) {throw 'Uninstall overwrote an unknown engine with a valid backup'}
$taskPass++; Write-Output 'PASS unsupported engine rejected without overwrite'

# Second phase: the disk states the installer window has to name correctly.
# Each state is built from a fresh copy of the pinned game, so a failure here
# cannot be a leftover of the case before it.
function Reset-Fixture {
 if (!(Test-TcChildPath -Parent (Join-Path $taskRepo 'build') -Child $taskFixture)) { throw 'Installer fixture escaped build' }
 Remove-Item -LiteralPath $taskFixture -Recurse -Force
 New-Item -ItemType Directory -Path $taskFixture | Out-Null
 Copy-Item (Join-Path $taskGame 'Turing Complete.exe') (Join-Path $taskFixture 'Turing Complete.exe') -Force
 Copy-Item $taskOriginal (Join-Path $taskFixture 'game_engine.dll') -Force
}

function Get-FixtureHash([string]$Name) {
 return (Get-FileHash (Join-Path $taskFixture $Name)).Hash
}

# A current install whose backup is gone must refuse to write, and must leave
# the installed loader exactly as it was.
Reset-Fixture
Invoke-SetupTest '--install' 0
$taskInstalled = Get-FixtureHash 'game_engine.dll'
Remove-Item -LiteralPath (Join-Path $taskFixture 'tc_game_engine.dll') -Force
Invoke-SetupTest '--install' 1
if ((Get-FixtureHash 'game_engine.dll') -ne $taskInstalled) {throw 'Missing backup did not block the write'}
$taskPass++; Write-Output 'PASS missing backup blocks install without touching the engine'

# A damaged backup is refused the same way.
Reset-Fixture
Invoke-SetupTest '--install' 0
$taskInstalled = Get-FixtureHash 'game_engine.dll'
[IO.File]::AppendAllText((Join-Path $taskFixture 'tc_game_engine.dll'),'damaged')
Invoke-SetupTest '--install' 1
if ((Get-FixtureHash 'game_engine.dll') -ne $taskInstalled) {throw 'Invalid backup did not block the write'}
Invoke-SetupTest '--uninstall' 1
if ((Get-FixtureHash 'game_engine.dll') -ne $taskInstalled) {throw 'Invalid backup did not block the uninstall'}
$taskPass++; Write-Output 'PASS invalid backup blocks install and uninstall'

# Uninstalling restores the pinned engine and leaves the mods folder and the
# loader's own data directory where they are.
Reset-Fixture
Invoke-SetupTest '--install' 0
New-Item -ItemType Directory -Force (Join-Path $taskFixture 'tc-modloader-data') | Out-Null
Set-Content -LiteralPath (Join-Path $taskFixture 'tc-modloader-data\marker.txt') -Value 'keep'
Invoke-SetupTest '--uninstall' 0
if ((Get-FixtureHash 'game_engine.dll') -ne $taskHash) {throw 'Uninstall did not restore the pinned engine'}
if (!(Test-Path (Join-Path $taskFixture 'mods'))) {throw 'Uninstall removed the mods folder'}
if (!(Test-Path (Join-Path $taskFixture 'tc-modloader-data\marker.txt'))) {throw 'Uninstall removed the loader data'}
$taskPass++; Write-Output 'PASS uninstall keeps mods and loader data'

# A directory that is not a game must fail without creating anything in it.
$taskNotAGame = Join-Path $taskRepo ('build\installer-empty-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskNotAGame | Out-Null
Copy-Item (Join-Path $taskGame 'Turing Complete.exe') (Join-Path $taskNotAGame 'Turing Complete.exe') -Force
$taskP=Start-HeadlessSetup '--install' $taskNotAGame
if ($taskP.ExitCode -ne 1) { throw "Unexpected setup exit for an incomplete game: $($taskP.ExitCode)" }
if (Test-Path (Join-Path $taskNotAGame 'tc_game_engine.dll')) {throw 'A backup was created outside a game directory'}
if (Test-Path (Join-Path $taskNotAGame 'mods')) {throw 'A mods folder was created outside a game directory'}
if (!(Test-TcChildPath -Parent (Join-Path $taskRepo 'build') -Child $taskNotAGame)) { throw 'Installer fixture escaped build' }
Remove-Item -LiteralPath $taskNotAGame -Recurse -Force
$taskPass++; Write-Output 'PASS an incomplete game directory is refused without side effects'

# A write the file system refuses must leave the engine exactly as it was, and
# must not leave the staging file behind for the player to puzzle over.
Reset-Fixture
Invoke-SetupTest '--install' 0
$taskInstalled = Get-FixtureHash 'game_engine.dll'
Set-ItemProperty -LiteralPath (Join-Path $taskFixture 'game_engine.dll') -Name IsReadOnly -Value $true
Invoke-SetupTest '--install' 1
Set-ItemProperty -LiteralPath (Join-Path $taskFixture 'game_engine.dll') -Name IsReadOnly -Value $false
if ((Get-FixtureHash 'game_engine.dll') -ne $taskInstalled) {throw 'A refused write changed the engine'}
if (Test-Path (Join-Path $taskFixture 'game_engine.dll.tc-tmp')) {throw 'A refused write left a staging file behind'}
Invoke-SetupTest '--uninstall' 0
if ((Get-FixtureHash 'game_engine.dll') -ne $taskHash) {throw 'Uninstall after a refused write did not restore the engine'}
$taskPass++; Write-Output 'PASS a refused write keeps the engine and cleans its staging file'

# Third phase: a loader that shipped in an earlier release has to be recognised
# as one of ours, so installing on top of it is an *upgrade* rather than the
# "未知引擎" refusal.  The fixture is the released installer's own embedded
# loader (resource 101) - exactly what a player who ran that release carries as
# game_engine.dll - with the pristine engine beside it as the backup that
# install had left.  A release that forgets to register its predecessor in
# compat/profiles.json fails here as a refused install (exit 1).
$taskReleases = Join-Path $taskRepo 'dist\releases'
$taskUpgrades = 0
if (Test-Path -LiteralPath $taskReleases) {
 $taskLoaderHash = (Get-FileHash -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Algorithm SHA256).Hash
 foreach ($taskRelease in (Get-ChildItem -LiteralPath $taskReleases -Directory | Sort-Object Name)) {
  $taskReleasedSetup = Join-Path $taskRelease.FullName 'TCModLoader-Setup.exe'
  if (!(Test-Path -LiteralPath $taskReleasedSetup)) { continue }
  Reset-Fixture
  [IO.File]::WriteAllBytes((Join-Path $taskFixture 'game_engine.dll'),
                           (Get-TcInstallerPayload -Installer $taskReleasedSetup))
  Copy-Item $taskOriginal (Join-Path $taskFixture 'tc_game_engine.dll') -Force
  $taskPreviousHash = Get-FixtureHash 'game_engine.dll'
  Invoke-SetupTest '--uninstall' 0
  if ((Get-FixtureHash 'game_engine.dll') -ne $taskHash -or
      (Test-Path (Join-Path $taskFixture 'tc_game_engine.dll'))) {
   throw "Uninstalling the $($taskRelease.Name) loader did not restore the original engine"
  }
  Write-Output "PASS direct uninstall of the published $($taskRelease.Name) loader"
  Reset-Fixture
  [IO.File]::WriteAllBytes((Join-Path $taskFixture 'game_engine.dll'),
                           (Get-TcInstallerPayload -Installer $taskReleasedSetup))
  Invoke-SetupTest '--uninstall' 1
  if ((Get-FixtureHash 'game_engine.dll') -ne $taskPreviousHash) {
   throw 'Uninstall changed an older loader without its backup'
  }
  Copy-Item $taskOriginal (Join-Path $taskFixture 'tc_game_engine.dll') -Force
  Invoke-SetupTest '--install' 0
  if ((Get-FixtureHash 'game_engine.dll') -ne $taskLoaderHash) {
   throw "Installing over the $($taskRelease.Name) loader did not deploy dist\tc-loader.dll"
  }
  $taskUpgrades++
  Write-Output "PASS upgrade from the published $($taskRelease.Name) loader"
 }
}
if ($taskUpgrades -eq 0) {
 Write-Output 'SKIP no released installer under dist\releases to upgrade from'
}
# The locally installed preview is also an upgrade path only when its exact
# hash was admitted to the compatibility profile. The real game stays read-only.
$taskLocalEngine = Join-Path $taskGame 'game_engine.dll'
$taskProfile = (Get-Content (Join-Path $taskRepo 'compat\profiles.json') -Raw | ConvertFrom-Json).profiles |
 Where-Object current -eq $true | Select-Object -First 1
$taskLocalHash = (Get-FileHash -LiteralPath $taskLocalEngine).Hash.ToLowerInvariant()
if (@($taskProfile.acceptedInstalledLoaders.sha256) -contains $taskLocalHash) {
 Reset-Fixture
 Copy-Item -LiteralPath $taskLocalEngine -Destination (Join-Path $taskFixture 'game_engine.dll') -Force
 Copy-Item $taskOriginal (Join-Path $taskFixture 'tc_game_engine.dll') -Force
 Invoke-SetupTest '--install' 0
 if ((Get-FixtureHash 'game_engine.dll') -ne (Get-FileHash (Join-Path $taskRepo 'dist\tc-loader.dll')).Hash) {
  throw 'The recognized local preview did not upgrade to the release loader'
 }
 Invoke-SetupTest '--uninstall' 0
 if ((Get-FixtureHash 'game_engine.dll') -ne $taskHash) { throw 'Preview upgrade did not preserve the original engine' }
 Write-Output 'PASS upgrade from the recognized local preview in an isolated copy'
}
"$taskPass installer tests passed" | Set-Content (Join-Path $taskRepo 'build\installer-test-results.txt')
