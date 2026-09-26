param([ValidateSet('persist','migrate','reject')][string]$Mode = 'persist')
# Drives tc.component.storage on the real game: the probe registers a component
# with a four-byte configuration, loads the level whose saved schematic carries
# an instance of it, writes the configuration through the service and saves with
# the game's own saver.  Several launches share one isolated profile, so each
# launch can only see what the previous one stored in the file.
#
#   persist - the fixture carries no record: write, save, read back.
#   migrate - the fixture carries a schema-6 record and the definition registers
#             schema 7: the migration converts it, and the next launch reads the
#             upgraded record without a migration.
#   reject  - the same fixture, but the migration refuses: the instance runs on
#             the default and the old bytes survive, which is why the next
#             launch is offered them again.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskMigrating = $Mode -ne 'persist'
$taskSchematic = Join-Path $taskRepo $(if($taskMigrating) { 'build\nl_not1legacy.data' } else { 'build\nl_not1board.data' })
if(!(Test-Path -LiteralPath $taskSchematic)) { throw "Missing $taskSchematic; run build.ps1 first" }

$taskTest = Join-Path $taskRepo ('build\component-storage-playtest-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'
$taskData = Join-Path $taskRoot 'tc-modloader-data\plugin-data\test.component-storage'
$taskPackage = Join-Path $taskTest 'package'
$taskSchema = Join-Path $taskProfile 'AppData\Roaming\Turing Complete Mods\profiles\default\schematics\not_gate\Default'
New-Item -ItemType Directory -Force $taskRoot,$taskProfile,$taskSchema,(Join-Path $taskRoot 'mods'),$taskData,(Join-Path $taskPackage 'native') | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath (Join-Path $taskRepo 'build\component-storage-probe.dll') -Destination (Join-Path $taskPackage 'native\probe.dll')
'{"format":2,"id":"test.component-storage","name":"Development component storage probe","version":"0.0.1","capabilities":["log","hook","services"],"native":{"api":1,"entry":"native/probe.dll"}}' | Set-Content -LiteralPath (Join-Path $taskPackage 'mod.json') -Encoding utf8
& (Join-Path $taskRepo 'tools\Pack-Mod.ps1') -Source $taskPackage -Output (Join-Path $taskRoot 'mods\probe.mod') | Out-Null
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply test.component-storage
if($LASTEXITCODE){throw 'Component storage probe package apply failed'}

$taskCircuit = Join-Path $taskSchema 'circuit.data'
Copy-Item -LiteralPath $taskSchematic -Destination $taskCircuit -Force
$taskInitialHash = (Get-FileHash -LiteralPath $taskCircuit).Hash
$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousMigrate = $env:TC_STORAGE_MIGRATE
# Per-launch expectations: the probe's own line, and the loader line that only
# that stage can produce (a record restored, a record converted, a record left
# alone).  Both are asserted after every launch, so a passing run cannot come
# from the memory-only path.
$taskExpected = @{
  persist = @(
    @{ Launch=1; Result='PASS storage launch=1 default value=11223344 stored=aabbccdd revision=1->2 persistence=1 config_changes=1'
       Log='has no stored configuration' }
    @{ Launch=2; Result='PASS storage launch=2 readback-first value=aabbccdd stored=01234567 revision=1->2 persistence=1 config_changes=1'
       Log='restored 4 configuration byte\(s\) from its saved record' }
    @{ Launch=3; Result='PASS storage launch=3 readback-second value=01234567 stored=01234567 revision=1->1 persistence=1 config_changes=0'
       Log='restored 4 configuration byte\(s\) from its saved record' }
  )
  migrate = @(
    @{ Launch=1; Result='PASS storage launch=1 migrated value=a55a0ff0 migrations=1 legacy=1 config_changes=0'
       Log='migrated its stored configuration from schema 6 \(4 bytes\) to schema 7 \(4 bytes\)' }
    @{ Launch=2; Result='PASS storage launch=2 upgraded-record value=a55a0ff0 migrations=0 legacy=0 config_changes=0'
       Log='restored 4 configuration byte\(s\) from its saved record' }
  )
  reject = @(
    @{ Launch=1; Result='PASS storage launch=1 refused value=11223344 migrations=1 legacy=1 config_changes=0'
       Log='refused to upgrade a stored configuration written under schema 6' }
    @{ Launch=2; Result='PASS storage launch=2 refused-again value=11223344 migrations=1 legacy=1 config_changes=0'
       Log='refused to upgrade a stored configuration written under schema 6' }
  )
}
$taskRuns = $taskExpected[$Mode]
# How often the load notification must have fired: a record that installed a
# configuration (or a migrated one) counts, an absent or refused record does not.
$taskExpectedLoads = @{
  persist = @{ 1 = 0; 2 = 1; 3 = 1 }
  migrate = @{ 1 = 1; 2 = 1 }
  reject  = @{ 1 = 0; 2 = 0 }
}
try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  if($Mode -eq 'persist') { Remove-Item Env:\TC_STORAGE_MIGRATE -ErrorAction SilentlyContinue }
  else { $env:TC_STORAGE_MIGRATE = if($Mode -eq 'migrate') { 'accept' } else { 'reject' } }
  foreach($taskCase in $taskRuns) {
    $taskRun = $taskCase.Launch
    $taskResult = Join-Path $taskData 'result.txt'
    if(Test-Path -LiteralPath $taskResult) { Remove-Item -LiteralPath $taskResult -Force }
    # A fresh log per launch, so a line the previous stage wrote cannot satisfy
    # this launch's assertion.
    $taskLogPath = Join-Path $taskRoot 'tc-modloader-data\loader.log'
    if(Test-Path -LiteralPath $taskLogPath) { Remove-Item -LiteralPath $taskLogPath -Force }
    $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
    $taskDeadline = [DateTime]::UtcNow.AddSeconds(40)
    while(!(Test-Path -LiteralPath $taskResult) -and !$taskProcess.HasExited -and [DateTime]::UtcNow -lt $taskDeadline) {
      Start-Sleep -Milliseconds 250
    }
    if(!(Test-Path -LiteralPath $taskResult)) {
      if(!$taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id }
      throw "Storage run $taskRun did not finish; inspect $taskRoot\tc-modloader-data\loader.log"
    }
    $taskText = [IO.File]::ReadAllText($taskResult)
    if(!$taskText.StartsWith('PASS ')) { throw "Storage run $taskRun failed: $taskText" }
    if(!$taskText.Contains($taskCase.Result)) {
      throw "Storage run $taskRun asserted '$($taskCase.Result)' but reported: $taskText"
    }
    # The lifecycle counters: `loads` says the configuration came out of the file
    # (a refused or absent record means it did not), and `saves` counts the moments
    # the game was about to write - the probe's own save plus the game's.
    $taskLoads = $taskExpectedLoads[$Mode][$taskRun]
    if($taskText -notmatch 'loads=(\d+) saves=(\d+)') {
      throw "Storage run $taskRun did not report its lifecycle counters: $taskText"
    }
    if([int]$Matches[1] -ne $taskLoads) {
      throw "Storage run $taskRun reported loads=$($Matches[1]), expected ${taskLoads}: $taskText"
    }
    if([int]$Matches[2] -lt 1) {
      throw "Storage run $taskRun never saw a save dispatch: $taskText"
    }
    if(!(Select-String -LiteralPath $taskLogPath -Pattern $taskCase.Log -Quiet)) {
      throw "Storage run $taskRun did not log '$($taskCase.Log)'; inspect $taskLogPath"
    }
    if(!$taskProcess.HasExited) {
      [void]$taskProcess.CloseMainWindow()
      if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id }
    }
    "run $taskRun`: $taskText"
  }
  # persist and migrate both change what the schematic carries; reject must not.
  if($Mode -ne 'reject' -and (Get-FileHash -LiteralPath $taskCircuit).Hash -eq $taskInitialHash) {
    throw "The first launch did not persist the configuration into the schematic (mode=$Mode)"
  }
  # The host has to say that it bound the record, otherwise a passing run could
  # come from the memory-only path.
  $taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
  if(!(Select-String -LiteralPath $taskLog -Pattern 'Component storage: configuration persists in the component record' -Quiet)) {
    throw "The loader never reported the persistent storage path; inspect $taskLog"
  }
  switch($Mode) {
    persist { 'PASS component configuration is written through tc.component.storage, saved with the schematic and restored after a restart' }
    migrate { 'PASS a schema-6 record is migrated to schema 7, committed to the record, and read back by the next launch without another migration' }
    reject  { 'PASS a refused migration changes neither the live configuration nor the stored record: the next launch is offered the same bytes again' }
  }
  "Evidence: $taskTest"
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  if($null -eq $taskPreviousMigrate) { Remove-Item Env:\TC_STORAGE_MIGRATE -ErrorAction SilentlyContinue }
  else { $env:TC_STORAGE_MIGRATE = $taskPreviousMigrate }
}
