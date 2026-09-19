# One-command manual test bench for the things that cannot be scripted:
# Chinese IME (composition string, candidate window placement) and whether
# typing into a circuit-board panel also reaches the board's own hotkeys.
#
# It prepares an isolated sandbox (own game copy, own USERPROFILE/APPDATA - the
# player's saves and mods are not touched), writes a checklist next to it, and
# starts the game in a *visible* window.
#
# Usage:   ./tests/manual-ime-test.ps1
#          ./tests/manual-ime-test.ps1 -Recreate            # rebuild the sandbox
#          ./tests/manual-ime-test.ps1 -Hidden              # smoke check only
#          ./tests/manual-ime-test.ps1 -Mods dev.ui-keyboard-manual,example.board-panel
param(
  [string]$Sandbox = (Join-Path (Split-Path $PSScriptRoot) 'build\manual-sandbox'),
  [string[]]$Mods = @('dev.ui-keyboard-manual'),
  [switch]$Recreate,
  # Start the game without showing a window: used to check that the bench
  # itself loads, not for the manual pass.
  [switch]$Hidden,
  [int]$SmokeSeconds = 20
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskGame = Join-Path $Sandbox 'game'
$taskLog = Join-Path $taskGame 'tc-modloader-data\loader.log'
$taskChecklist = Join-Path $Sandbox 'CHECKLIST.txt'

& (Join-Path $PSScriptRoot 'make-ui-sandbox.ps1') -Sandbox $Sandbox -Mods $Mods -Recreate:$Recreate

$taskText = @'
键盘 / 中文输入法 人工验收（自动准备工作已完成）

准备工作已经做完：这是隔离副本，独立 USERPROFILE/APPDATA，不会动你自己的存档与 mods。
关掉游戏后，把同目录 game\tc-modloader-data\loader.log 发回给 Codex 即可。

A. 主菜单页面里的中文输入
   1) 主菜单点 Keyboard probe 进入页面，点一下文本框；
   2) 切微软拼音，输入 nihao：组合串应出现在文本框里；
   3) 看候选窗位置：是否贴在文本框附近？（跑到屏幕角落/左上角 = 定位没跟随控件）
      探针会把系统要求的位置写进日志：Keyboard probe: IME page composition=… candidate=…；
   4) 空格/数字选词上屏：内容正确、只出现一次；
   5) 输入法还开着时按 Esc：应取消组合，页面不应关闭；
   6) 顺带试中文标点和全角字符。

B. 电路板侧栏面板里打字 + 快捷键穿透
   1) 进任意关卡，右上角出现 Board keys probe 面板，点进它的文本框；
   2) 拼音输入并上屏 —— 关键问题：打字时电路板有没有同时反应？
      （字母被当成元件选择 / 空格让仿真运行 / Esc 弹菜单）
      日志里的 board panel saw key X 只说明面板收到了键，板上是否也吃到要看画面；
   3) 输入法开着按 Esc：取消组合 / 关面板 / 被游戏菜单接走，是哪种；
   4) 观感：面板被裁到边界内吗？右缘滚动条拖动顺手吗？面板挡住游戏工具栏了吗？

C. 显示相关（完全未测）
   多显示器或系统缩放不是 100% 时，页面/面板的位置、字号、鼠标点击是否仍然对得上；
   分辨率不是 2560x1600 时面板尺寸是否合适。

已经自动化、不必手工重复：注册/绘制/生命周期、真实鼠标点击不穿透、裁剪与滚动条拖动、
按键与字符到达、Escape 被输入框消费、UTF-8 中文码元进入缓冲区。
'@
Set-Content -LiteralPath $taskChecklist -Value $taskText -Encoding utf8

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
try {
  $env:USERPROFILE = Join-Path $Sandbox 'home'
  $env:APPDATA = Join-Path $env:USERPROFILE 'AppData\Roaming'
  $taskArguments = @{
    FilePath = Join-Path $taskGame 'Turing Complete.exe'
    WorkingDirectory = $taskGame
    PassThru = $true
  }
  if ($Hidden) { $taskArguments.WindowStyle = 'Hidden' }
  $taskProcess = Start-Process @taskArguments
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
}

if ($Hidden) {
  Start-Sleep -Seconds $SmokeSeconds
  if (!$taskProcess.HasExited) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id }
  }
  $taskLines = if (Test-Path -LiteralPath $taskLog) { Get-Content -LiteralPath $taskLog } else { @() }
  $taskBad = $taskLines | Where-Object { $_ -match 'Native failed|Plugin callback threw|registerBoardPanel returned' }
  if ($taskBad) { $taskBad | Select-Object -First 5; throw 'The manual bench reported an error' }
  ($taskLines | Where-Object { $_ -match 'Keyboard probe' }) | Select-Object -First 4
  "PASS manual bench: mods loaded, probe registered (no window shown)"
  "Sandbox: $Sandbox"
  return
}

"Sandbox: $Sandbox"
"Checklist: $taskChecklist   (also printed below)"
"Log to send back when done: $taskLog"
""
Get-Content -LiteralPath $taskChecklist
