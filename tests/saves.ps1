$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskTest=Join-Path $taskRepo ('build\save-test-中文-'+[guid]::NewGuid().ToString('N'))
$taskHome=Join-Path $taskTest 'home'
$taskRoot=Join-Path $taskTest 'game'
$taskOriginal=Join-Path $taskHome 'AppData\Roaming\Turing Complete'
$taskMod=Join-Path $taskHome 'AppData\Roaming\Turing Complete Mods\profiles\default'
New-Item -ItemType Directory -Force (Join-Path $taskOriginal 'schematics\中文电路'),$taskMod,$taskRoot | Out-Null
[IO.File]::WriteAllText((Join-Path $taskOriginal 'levels.txt'),'original-progress')
[IO.File]::WriteAllBytes((Join-Path $taskOriginal 'schematics\中文电路\circuit.data'),[byte[]](0,1,255,13,10))
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
 if((Get-FileHash -LiteralPath (Join-Path $taskFirst 'schematics\中文电路\circuit.data')).Hash -ne (Get-FileHash -LiteralPath (Join-Path $taskOriginal 'schematics\中文电路\circuit.data')).Hash){throw 'Binary schematic differs'}
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
 $taskOutput=& $taskCli $taskRoot import-saves 2>&1
 if(!$LASTEXITCODE){throw 'Linked source accepted'}
 if([IO.File]::ReadAllText($taskConfig) -ne $taskConfigBefore){throw 'Failed import changed selection'}
 'PASS linked source rejected without switching profile'
 [IO.File]::WriteAllText($taskConfig,"[saves]`r`nprofile=../escape`r`n")
 $taskOutput=& $taskCli $taskRoot save-path 2>&1
 if(!$LASTEXITCODE){throw 'Invalid profile accepted'}
 'PASS traversal profile rejected'
} finally {$env:USERPROFILE=$taskPrevious}
