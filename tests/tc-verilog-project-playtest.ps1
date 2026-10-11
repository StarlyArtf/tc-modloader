$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Split-Path $taskRepo
$taskTest = Join-Path $taskRepo ('build\tc-verilog-project-' + [guid]::NewGuid().ToString('N'))
$taskRoot = Join-Path $taskTest 'game'
$taskProfile = Join-Path $taskTest 'home'

New-Item -ItemType Directory -Force $taskRoot,$taskProfile,(Join-Path $taskRoot 'mods') | Out-Null
foreach($taskDir in @('asset','campaign','translations')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskDir) -Destination $taskRoot -Recurse
}
foreach($taskFile in @('Turing Complete.exe','compile.dll','tc_game_engine.dll','soft_oal.dll','steam_api64.dll','libgcc_s_seh-1.dll','libwinpthread-1.dll')) {
  Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot
}
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\tc-loader.dll') -Destination (Join-Path $taskRoot 'game_engine.dll')
Copy-Item -LiteralPath (Join-Path $taskRepo 'dist\example.tc-verilog-ram.mod') -Destination (Join-Path $taskRoot 'mods')
& (Join-Path $taskRepo 'dist\tcmod-cli.exe') $taskRoot apply example.tc-verilog-ram
if($LASTEXITCODE) { throw 'TC-Verilog example package apply failed' }

$taskOldProfile = $env:USERPROFILE
$taskOldAppData = $env:APPDATA
$taskOldOpenPage = $env:TC_MODLOADER_OPEN_PLUGIN_PAGE
$taskOldShot = $env:TC_MODLOADER_SHOT
$taskOldShotDelay = $env:TC_MODLOADER_SHOT_DELAY
$taskOldEditorFocus = $env:TC_VERILOG_EDITOR_FOCUS
$taskOldCreateTarget = $env:TC_VERILOG_CREATE_TARGET
$taskProjectsView = $env:TC_VERILOG_PROJECTS_VIEW -and $env:TC_VERILOG_PROJECTS_VIEW[0] -ne '0'
$taskCreateView = $env:TC_VERILOG_CREATE_VIEW -and $env:TC_VERILOG_CREATE_VIEW[0] -ne '0'
$taskCreateTest = $env:TC_VERILOG_CREATE_TEST -and $env:TC_VERILOG_CREATE_TEST[0] -ne '0'
$taskVisualOnly = $env:TC_VERILOG_VISUAL_ONLY -and $env:TC_VERILOG_VISUAL_ONLY[0] -ne '0'
function Start-TestGame {
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds 35
  if(!$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if(!$taskProcess.WaitForExit(3000)) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
  }
}

try {
  $env:USERPROFILE = $taskProfile
  $env:APPDATA = Join-Path $taskProfile 'AppData\Roaming'
  $taskEditorShot = Join-Path $taskTest 'tc-verilog-editor.bmp'
  $env:TC_MODLOADER_OPEN_PLUGIN_PAGE = 'example.tc-verilog-ram/tc-verilog'
  $env:TC_MODLOADER_SHOT = $taskEditorShot
  $env:TC_MODLOADER_SHOT_DELAY = '12000'
  # The page focuses its source field by itself when it opens; the switch is
  # pinned off so the capture shows the default path, not the developer aid.
  $env:TC_VERILOG_EDITOR_FOCUS = '0'
  if($taskCreateTest) {
    $taskCreatedProject = Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.tc-verilog-ram\created-ui-project'
    $env:TC_VERILOG_CREATE_TARGET = $taskCreatedProject
  }
  Start-TestGame
  $env:TC_MODLOADER_OPEN_PLUGIN_PAGE = $null
  $env:TC_MODLOADER_SHOT = $null
  $env:TC_MODLOADER_SHOT_DELAY = $null
  $env:TC_VERILOG_EDITOR_FOCUS = $null
  $env:TC_VERILOG_CREATE_TARGET = $null

  $taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
  $taskWorkspace = Join-Path $taskRoot 'tc-modloader-data\plugin-data\example.tc-verilog-ram\tc-verilog'
  $taskSource = Join-Path $taskWorkspace 'ram16x8.v'
  $taskManifest = Join-Path $taskWorkspace 'tc-verilog.json'
  if(!(Test-Path -LiteralPath $taskLog) -or !(Test-Path -LiteralPath $taskSource) -or
     !(Test-Path -LiteralPath $taskManifest) -or !(Test-Path -LiteralPath $taskEditorShot) -or
     (Get-Item -LiteralPath $taskEditorShot).Length -lt 65536) {
    throw "TC-Verilog workspace was not materialised; inspect $taskTest"
  }
  if($taskCreateTest -and
     (!(Test-Path -LiteralPath (Join-Path $taskCreatedProject 'tc-verilog.json')) -or
      !(Test-Path -LiteralPath (Join-Path $taskCreatedProject 'src\main.v')))) {
    throw "The in-game new-project flow did not create its manifest and source; inspect $taskTest"
  }
  $taskFirst = Get-Content -LiteralPath $taskLog -Raw
  if($taskFirst -notmatch 'Native logic: registered custom 0x54435652414d3031 inputs=5 outputs=1' -or
     $taskFirst -notmatch "UI page registered: tc-verilog" -or
     $taskFirst -notmatch "tc-verilog-ram: registered project from .v source and editor page 'tc-verilog'" -or
     $taskFirst -notmatch "tc-verilog-ram: live component registration available" -or
     $taskFirst -notmatch "tc-verilog-ram: source editor has line numbers, syntax highlighting and auto indent") {
    throw "TC-Verilog project or editor page was not registered; inspect $taskLog"
  }
  if($taskCreateTest -and $taskFirst -notmatch 'tc-verilog-ram: opened project .*created-ui-project\\tc-verilog.json') {
    throw "The newly created project was not opened and registered; inspect $taskLog"
  }

  if(!$taskProjectsView -and !$taskCreateView -and !$taskCreateTest) {
    # The captured frame must really contain the editor's own palette, not just a
    # plain text box: the keyword and number colours are drawn as whole pixels and
    # land in the BMP in BGRA order.
    $taskShotBytes = [System.IO.File]::ReadAllBytes($taskEditorShot)
    $taskShotText = [System.Text.Encoding]::GetEncoding(28591).GetString($taskShotBytes)
    $taskKeywords = ([regex]::Matches($taskShotText,
       [string][char]0xff + [char]0xa2 + [char]0x7d + [char]0xff)).Count
    $taskNumbers = ([regex]::Matches($taskShotText,
       [string][char]0x86 + [char]0xd0 + [char]0xb0 + [char]0xff)).Count
    if($taskKeywords -lt 500 -or $taskNumbers -lt 60) {
      throw "The editor screenshot has no syntax highlighting (keyword=$taskKeywords number=$taskNumbers); inspect $taskEditorShot"
    }
  }

  if(!$taskProjectsView -and !$taskCreateView -and !$taskCreateTest -and !$taskVisualOnly) {
    @('module broken(input a, output y);','this is deliberately invalid','endmodule') |
      Set-Content -LiteralPath $taskSource -Encoding ascii
    Remove-Item -LiteralPath $taskLog -Force
    Start-TestGame
    $taskSecond = Get-Content -LiteralPath $taskLog -Raw
    if($taskSecond -notmatch 'tc-verilog-ram: compile:' -or
       $taskSecond -notmatch 'ram16x8.v:2:' -or
       $taskSecond -notmatch '\[example.tc-verilog-ram/ram16x8\]') {
      throw "Broken workspace source did not produce a located compiler diagnostic; inspect $taskLog"
    }
    if((Get-Content -LiteralPath $taskSource -Raw) -notmatch 'deliberately invalid') {
      throw 'Second launch overwrote the player-edited Verilog source'
    }
  }
} finally {
  $env:USERPROFILE = $taskOldProfile
  $env:APPDATA = $taskOldAppData
  $env:TC_MODLOADER_OPEN_PLUGIN_PAGE = $taskOldOpenPage
  $env:TC_MODLOADER_SHOT = $taskOldShot
  $env:TC_MODLOADER_SHOT_DELAY = $taskOldShotDelay
  $env:TC_VERILOG_EDITOR_FOCUS = $taskOldEditorFocus
  $env:TC_VERILOG_CREATE_TARGET = $taskOldCreateTarget
}

if($taskProjectsView) {
  "PASS TC-Verilog project selector: project page rendered and workspace persisted"
} elseif($taskCreateView) {
  "PASS TC-Verilog new project: creation form rendered"
} elseif($taskCreateTest) {
  "PASS TC-Verilog new project: manifest and source created, opened, and registered in-game"
} elseif($taskVisualOnly) {
  "PASS TC-Verilog editor visual: real project tree and syntax-highlighted editor rendered"
} else {
  "PASS TC-Verilog project: V3 component and editor page registered with a syntax highlighted source editor, workspace persisted, and invalid edits reported with file/line"
}
"Sandbox: $taskTest"
