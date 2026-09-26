$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)
$OutputEncoding=[Console]::OutputEncoding
$taskRepo=Split-Path $PSScriptRoot
$taskUnicode=([string][char]0x4e2d)+([char]0x6587)
$taskCircuitName=$taskUnicode+([char]0x7535)+([char]0x8def)
$taskTest=Join-Path $taskRepo ('build\save-test-'+$taskUnicode+'-'+[guid]::NewGuid().ToString('N'))
$taskHome=Join-Path $taskTest 'home'
$taskRoot=Join-Path $taskTest 'game'
$taskOriginal=Join-Path $taskHome 'AppData\Roaming\Turing Complete'
$taskMod=Join-Path $taskHome 'AppData\Roaming\Turing Complete Mods\profiles\default'
$taskProfiles=Split-Path -Parent $taskMod
$taskModName=Split-Path -Leaf $taskMod
New-Item -ItemType Directory -Force (Join-Path $taskOriginal ('schematics\'+$taskCircuitName)),$taskMod,$taskRoot | Out-Null
[IO.File]::WriteAllText((Join-Path $taskOriginal 'levels.txt'),'original-progress')
[IO.File]::WriteAllBytes((Join-Path $taskOriginal ('schematics\'+$taskCircuitName+'\circuit.data')),[byte[]](0,1,255,13,10))
[IO.File]::WriteAllText((Join-Path $taskOriginal 'steam_autocloud.vdf'),'do-not-copy-cloud-metadata')
[IO.File]::WriteAllText((Join-Path $taskMod 'levels.txt'),'existing-modded-progress')
function Snapshot($p){return ((Get-ChildItem -LiteralPath $p -Recurse -File | Sort-Object FullName | ForEach-Object { $_.FullName.Substring($p.Length)+':'+(Get-FileHash -LiteralPath $_.FullName).Hash }) -join "`n")}
$taskBefore=Snapshot $taskOriginal
$taskPrevious=$env:USERPROFILE
try {
 $env:USERPROFILE=$taskHome
 $taskCli=Join-Path $taskRepo 'dist\tcmod-cli.exe'
 & $taskCli $taskRoot import-saves
 if($LASTEXITCODE){throw 'Import failed'}
 $taskFirst=(& $taskCli $taskRoot save-path).Trim()
 if((Get-Content -LiteralPath (Join-Path $taskFirst 'levels.txt') -Raw) -ne 'original-progress'){throw 'Progress differs'}
 if((Get-FileHash -LiteralPath (Join-Path $taskFirst ('schematics\'+$taskCircuitName+'\circuit.data'))).Hash -ne (Get-FileHash -LiteralPath (Join-Path $taskOriginal ('schematics\'+$taskCircuitName+'\circuit.data'))).Hash){throw 'Binary schematic differs'}
 if(Test-Path -LiteralPath (Join-Path $taskFirst 'steam_autocloud.vdf')){throw 'Cloud metadata copied'}
 'PASS complete import including Unicode and binary schematics; cloud metadata excluded'
 [IO.File]::WriteAllText((Join-Path $taskFirst 'levels.txt'),'mod-progress-1')
 & $taskCli $taskRoot import-saves
 if($LASTEXITCODE){throw 'Second import failed'}
 $taskSecond=(& $taskCli $taskRoot save-path).Trim()
 if($taskFirst -eq $taskSecond){throw 'Import reused profile'}
 if((Get-Content -LiteralPath (Join-Path $taskFirst 'levels.txt') -Raw) -ne 'mod-progress-1'){throw 'Previous import overwritten'}
 if((Get-Content -LiteralPath (Join-Path $taskMod 'levels.txt') -Raw) -ne 'existing-modded-progress'){throw 'Default overwritten'}
 if((Snapshot $taskOriginal) -ne $taskBefore){throw 'Original modified'}
 'PASS repeated imports preserve original, default and previous imported profiles'
 $taskConfig=Join-Path $taskRoot 'tc-modloader-data\saves.ini'
 $taskConfigBefore=[IO.File]::ReadAllText($taskConfig)
 New-Item -ItemType Junction -Path (Join-Path $taskOriginal 'linked') -Target $taskMod | Out-Null
 $taskPreviousErrorAction=$ErrorActionPreference
 $ErrorActionPreference='Continue'
 $taskOutput=& $taskCli $taskRoot import-saves 2>&1
 $taskExpectedExit=$LASTEXITCODE
 $ErrorActionPreference=$taskPreviousErrorAction
 if(!$taskExpectedExit){throw 'Linked source accepted'}
 if([IO.File]::ReadAllText($taskConfig) -ne $taskConfigBefore){throw 'Failed import changed selection'}
 'PASS linked source rejected without switching profile'
 [IO.File]::WriteAllText($taskConfig,"[saves]`r`nprofile=../escape`r`n")
 $ErrorActionPreference='Continue'
 $taskOutput=& $taskCli $taskRoot save-path 2>&1
 $taskExpectedExit=$LASTEXITCODE
 $ErrorActionPreference=$taskPreviousErrorAction
 if(!$taskExpectedExit){throw 'Invalid profile accepted'}
 'PASS traversal profile rejected'
 # The management operations the 存档 page drives, through the same core (the
 # page and tcmod-cli saves ... share SaveProfiles, so this is where they can be
 # exercised without synthetic mouse input).
 & $taskCli $taskRoot saves switch $taskModName | Out-Null
 if($LASTEXITCODE){throw 'Switch failed'}
 $taskList=(& $taskCli $taskRoot saves list) -join "`n"
 if($taskList -notmatch '(?m)^'+[regex]::Escape($taskModName)+' \| running \|'){
  throw "List did not report the selected profile as running`n$taskList"
 }
 $taskCreated=(& $taskCli $taskRoot saves create) -join ''
 $taskCreatedName=[IO.Path]::GetFileName((& $taskCli $taskRoot save-path).Trim())
 if($taskCreatedName -notmatch '^local-'){throw "New profile is not a local- album: $taskCreatedName"}
 if((Get-Content -LiteralPath (Join-Path $taskFirst 'levels.txt') -Raw) -ne "mod-progress-1"){
  throw 'Creating a profile disturbed an existing one'
 }
 'PASS new profile created and selected, existing profiles untouched'
 $taskCopy=[IO.Path]::GetFileName(((& $taskCli $taskRoot saves duplicate $taskModName) | Out-String).Trim())
 if((Get-Content -LiteralPath (Join-Path (Join-Path $taskProfiles $taskCopy) 'levels.txt') -Raw) -ne 'existing-modded-progress'){
  throw "Duplicated profile does not carry the source contents ($taskCopy)"
 }
 & $taskCli $taskRoot saves rename $taskCopy renamed-copy | Out-Null
 if($LASTEXITCODE){throw 'Rename failed'}
 if(!(Test-Path -LiteralPath (Join-Path $taskProfiles 'renamed-copy\levels.txt'))){throw 'Renamed profile missing'}
 & $taskCli $taskRoot saves remove renamed-copy | Out-Null
 if($LASTEXITCODE){throw 'Remove failed'}
 if(Test-Path -LiteralPath (Join-Path $taskProfiles 'renamed-copy')){throw 'Removed profile still in place'}
 $taskTrash=@(Get-ChildItem -LiteralPath $taskProfiles -Directory | Where-Object { $_.Name -like 'trash-renamed-copy-*' })
 if($taskTrash.Count -ne 1){throw 'Removed profile did not land in a trash- profile'}
 if((Get-Content -LiteralPath (Join-Path $taskTrash[0].FullName 'levels.txt') -Raw) -ne 'existing-modded-progress'){
  throw 'Trashed profile lost its contents'
 }
 'PASS duplicate, rename and remove-to-trash keep the data recoverable'
 $taskSelectedBefore=(& $taskCli $taskRoot save-path).Trim()
 $ErrorActionPreference='Continue'
 & $taskCli $taskRoot saves remove $taskCreatedName 2>&1 | Out-Null
 $taskRefused=$LASTEXITCODE
 $ErrorActionPreference=$taskPreviousErrorAction
 if(!$taskRefused){throw 'Removing the selected profile was allowed'}
 if((& $taskCli $taskRoot save-path).Trim() -ne $taskSelectedBefore){throw 'Refused removal changed the selection'}
 'PASS the profile this start would use cannot be removed'
 # Dependency scan: a circuit carries only each custom component's 64 bit id, so
 # the report joins the ids found in the profile with the registry cache the
 # loader built while Mods registered.  Frame a literal-only Snappy circuit by
 # hand (the same encoding tests/float-fixture.cpp writes) so this stays offline.
 function New-TaskCircuit([string]$Path,[string[]]$Tags){
  $raw = New-Object System.Collections.Generic.List[byte]
  foreach ($tag in $Tags) {
   # The game stores the id little-endian, so a tag spelled "F32ABS_1" appears in
   # the file as its reverse; twice, because the scan asks for a second sighting.
   $bytes = [Text.Encoding]::ASCII.GetBytes(-join $tag.ToCharArray()[($tag.Length - 1)..0])
   # Two sightings, each delimited: the scan only accepts a tag that is not part
   # of a longer printable run, exactly how the game file separates them.
   foreach ($sighting in 1..2) { $raw.Add([byte]0); $raw.AddRange($bytes) }
  }
  $out = New-Object System.Collections.Generic.List[byte]
  $length = $raw.Count
  do { $byte = [byte]($length -band 0x7f); $length = $length -shr 7
       if ($length) { $byte = $byte -bor 0x80 }; $out.Add($byte) } while ($length)
  for ($i = 0; $i -lt $raw.Count;) {
   $count = [Math]::Min(60, $raw.Count - $i)
   $out.Add([byte](($count - 1) -shl 2))
   $out.AddRange($raw.GetRange($i, $count))
   $i += $count
  }
  New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
  $file = New-Object System.Collections.Generic.List[byte]
  $file.Add([byte]13)
  $file.AddRange($out)
  [IO.File]::WriteAllBytes($Path, $file.ToArray())
 }
 $taskScanProfile = Join-Path $taskProfiles 'scan-me'
 New-TaskCircuit (Join-Path $taskScanProfile 'schematics\level\Default\circuit.data') @('F32ABS_1','TXT_0001')
 $taskRegistry = Join-Path $taskRoot 'tc-modloader-data\registry.json'
 [IO.File]::WriteAllText($taskRegistry, '{"4633324142535f31":{"mod":"local.float-ops","name":"FP32 Absolute","type":"local.float-ops/abs","version":"0.2.0","digest":"deadbeef","seen":"2026-09-27","schema":3}}')
 $taskReport = (& $taskCli $taskRoot saves report scan-me) -join "`n"
 if($LASTEXITCODE){throw 'Dependency report failed'}
 if($taskReport -notmatch 'circuits=1'){
  throw "The scan did not read the crafted circuit`n$taskReport"
 }
 if($taskReport -notmatch 'type 0x4633324142535f31 F32ABS_1 \| FP32 Absolute \| mod=local.float-ops'){
  throw "The registry did not name the type or its Mod`n$taskReport"
 }
 if($taskReport -notmatch 'type 0x5458545f30303031 TXT_0001 \| unknown'){
  throw "The unknown tag fallback did not fire`n$taskReport"
 }
 if($taskReport -notmatch 'summary missing=2 disabled=0 unknown=1'){
  throw "Missing/unknown counts are wrong`n$taskReport"
 }
 'PASS dependency report names the Mod from the registry cache and flags what is unknown'
 $taskReportUnscanned = (& $taskCli $taskRoot saves report $taskModName) -join "`n"
 if($taskReportUnscanned -notmatch 'summary missing=0 disabled=0 unknown=0'){
  throw "A profile without custom components reported dependencies`n$taskReportUnscanned"
 }
 'PASS a profile with no custom components reports no dependencies'
} finally {$env:USERPROFILE=$taskPrevious}
