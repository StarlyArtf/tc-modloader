# Keyboard and character-input playtest for plugin pages.
#
# What it proves (and what it deliberately does not):
#   * a plugin's InputText receives key presses and characters through the
#     game's own input path - the messages are real WM_KEYDOWN/UP and WM_CHAR
#     posted to the game window, with proper scancodes;
#   * Tab and Escape reach the page's widgets (the probe logs what it sees);
#   * a Chinese code unit sent as WM_CHAR and as WM_IME_CHAR tells us which of
#     the two paths this build forwards to ImGui.
# It does NOT drive a real Chinese IME composition (candidate window, preedit):
# that is not scriptable, and the manual checklist lives in docs/sdk/ui.md.
#
# Two phases, like the other page playtests: -Probe records the loader's page
# entry rectangle into <sandbox>\keyboard-entry.txt, then the driver run types
# into the page it opens.
param(
  [string]$Sandbox = (Join-Path (Split-Path $PSScriptRoot) 'build\keyboard-sandbox'),
  [int]$Seconds = 45,
  [switch]$Probe,
  [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path $PSScriptRoot
$taskRoot = Join-Path $Sandbox 'game'
$taskLog = Join-Path $taskRoot 'tc-modloader-data\loader.log'
$taskEntry = Join-Path $Sandbox 'keyboard-entry.txt'
$taskMod = 'dev.ui-keyboard-probe'

# Always refresh the sandbox: make-ui-sandbox.ps1 is what copies the loader
# under test into it, and a stale loader silently tests the previous build.
& (Join-Path $PSScriptRoot 'make-ui-sandbox.ps1') -Sandbox $Sandbox -Mods $taskMod
Remove-Item -LiteralPath $taskLog -ErrorAction SilentlyContinue

$taskPreviousProfile = $env:USERPROFILE
$taskPreviousAppData = $env:APPDATA
$taskPreviousEntry = $env:TC_KEY_ENTRY
$taskProcess = $null
$taskText = ''
try {
  if ($Probe) {
    $env:TC_KEY_ENTRY = $null
  } else {
    if (!(Test-Path -LiteralPath $taskEntry)) {
      throw "Missing $taskEntry; run this playtest with -Probe first"
    }
    $env:TC_KEY_ENTRY = (Get-Content -LiteralPath $taskEntry -Raw).Trim()
    "Driver will click the page entry at: $($env:TC_KEY_ENTRY)"
  }
  $env:USERPROFILE = Join-Path $Sandbox 'home'
  $env:APPDATA = Join-Path $Sandbox 'home\AppData\Roaming'
  $taskProcess = Start-Process -FilePath (Join-Path $taskRoot 'Turing Complete.exe') `
    -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru
  $taskDeadline = (Get-Date).AddSeconds($Seconds)
  $taskStop = if ($Probe) {
    'Keyboard probe: registered page|Native failed|Plugin callback threw'
  } else {
    'DRIVER: done|DRIVER: giving up|Native failed|Plugin callback threw|UI page disabled'
  }
  do {
    Start-Sleep -Milliseconds 500
    $taskText = if (Test-Path -LiteralPath $taskLog) { Get-Content -LiteralPath $taskLog -Raw } else { '' }
    if ($taskText -match $taskStop -or $taskProcess.HasExited) { break }
  } while ((Get-Date) -lt $taskDeadline)
} finally {
  $env:USERPROFILE = $taskPreviousProfile
  $env:APPDATA = $taskPreviousAppData
  $env:TC_KEY_ENTRY = $taskPreviousEntry
  if ($taskProcess -and !$taskProcess.HasExited -and !$KeepRunning) {
    [void]$taskProcess.CloseMainWindow()
    if (!$taskProcess.WaitForExit(4000)) { Stop-Process -Id $taskProcess.Id }
  }
}

if (!(Test-Path -LiteralPath $taskLog)) { throw "Loader log missing: $taskLog" }
$taskLines = Get-Content -LiteralPath $taskLog
$taskText = $taskLines -join "`n"
$taskFatal = $taskLines | Where-Object {
  $_ -match 'Native failed|Plugin callback threw|UI page disabled|UI 页面异常|回调异常'
}
if ($taskFatal) { $taskFatal | Select-Object -First 8; throw 'The keyboard probe reported an error' }

if ($Probe) {
  # The loader logs the hit box of every page entry it draws; record the first
  # one (there is exactly one page in this package).
  $taskButton = $taskLines | Where-Object {
    $_ -match 'home button (\S+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) game=(\d+)x(\d+) window=(\d+)x(\d+)' -and
    $Matches[1] -ne 'Mods'
  } | Select-Object -First 1
  if (!$taskButton) { $taskLines | Select-Object -Last 20; throw 'The loader never logged a page entry rectangle' }
  $null = $taskButton -match 'home button (\S+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) game=(\d+)x(\d+) window=(\d+)x(\d+)'
  $taskReport = '{0} {1} {2} {3} {4} {5} {6}' -f $Matches[1],
    ([int]$Matches[2] + [int]$Matches[4] / 2), ([int]$Matches[3] + [int]$Matches[5] / 2),
    $Matches[6], $Matches[7], $Matches[8], $Matches[9]
  Set-Content -LiteralPath $taskEntry -Value $taskReport -Encoding ascii
  $taskButton
  "PASS keyboard probe: page entry recorded into $taskEntry ($taskReport)"
  "Sandbox: $Sandbox"
  return
}

foreach ($taskExpected in @(
    'Keyboard probe: registered page ''keys''',
    'DRIVER: page visible',
    'Keyboard probe: page first drawn on frame',
    'DRIVER: typing "abc" as key messages',
    'DRIVER: pressing Tab',
    'DRIVER: sending ''z'' as WM_CHAR only',
    'DRIVER: pressing Escape',
    'DRIVER: done')) {
  if (!$taskText.Contains($taskExpected)) {
    $taskLines | Select-Object -Last 25
    throw "Missing evidence: $taskExpected"
  }
}

# Character delivery: the plugin's own log line is the evidence, so it does not
# matter which backend path carried the characters.  "abc" must arrive exactly,
# without a duplicate from the key messages.
$taskAscii = $taskLines | Where-Object { $_ -match 'Keyboard probe: buffer "abc" length=3' } |
  Select-Object -First 1
if (!$taskAscii) {
  ($taskLines | Where-Object { $_ -match 'Keyboard probe: buffer' }) | Select-Object -First 8
  throw 'The ASCII characters typed by the driver never reached the plugin InputText'
}

# The character path on its own: one WM_CHAR and no key message -> 'z'.
$taskCharOnly = $taskLines | Where-Object {
  $_ -match 'Keyboard probe: buffer "abc\\xe4\\xbd\\xa0\\xe5\\xa5\\xbdd?z" length=(10|11)'
} |
  Select-Object -First 1
if (!$taskCharOnly) {
  ($taskLines | Where-Object { $_ -match 'Keyboard probe: buffer' }) | Select-Object -First 8
  throw 'A WM_CHAR sent without a key message did not reach the InputText exactly once'
}

# Non-ASCII: 你 (WM_CHAR) and 好 (WM_IME_CHAR) as UTF-8 in the same buffer.
$taskChinese = $taskLines | Where-Object {
  $_ -match 'Keyboard probe: buffer "abc\\xe4\\xbd\\xa0\\xe5\\xa5\\xbd" length=9'
} | Select-Object -First 1
if (!$taskChinese) {
  ($taskLines | Where-Object { $_ -match 'Keyboard probe: buffer' }) | Select-Object -First 8
  throw 'The Chinese code units never reached the plugin InputText'
}
# The exact buffer contents above are also the "no doubling" check: every stage
# shows precisely the characters that were sent, in order, with no repeat.

# Key delivery: Tab and Escape have to be visible to the page's widgets.
$taskKeys = $taskLines | Where-Object { $_ -match 'Keyboard probe: key (Tab|Enter|Escape|Space|A|F5) (pressed|down|up)' }
if (!($taskKeys | Where-Object { $_ -match 'key Tab' })) {
  $taskKeys | Select-Object -First 8
  throw 'Tab never reached the page'
}
if (!($taskKeys | Where-Object { $_ -match 'key Escape' })) {
  $taskKeys | Select-Object -First 8
  throw 'Escape never reached the page'
}

# Focus movement: reported by the probe when a widget gains or loses focus.
$taskFocus = $taskLines | Where-Object { $_ -match 'Keyboard probe: focus (text|second|flag)=[01]' }

$taskKeys | Select-Object -First 6
$taskFocus | Select-Object -First 6
($taskLines | Where-Object { $_ -match 'Keyboard probe: buffer' }) | Select-Object -First 8
$taskSummary = 'PASS keyboard probe: keys reach plugin widgets; a key press delivers exactly '
$taskSummary += 'one character (Windows turns the key into WM_CHAR - posting both doubles it), '
$taskSummary += 'a bare WM_CHAR delivers exactly one, and UTF-8 for 你 and 好 arrives intact'
$taskSummary
"Sandbox: $Sandbox"
