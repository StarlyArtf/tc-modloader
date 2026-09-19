Set-StrictMode -Version 2.0

function Get-TcFullPath {
  param([Parameter(Mandatory = $true)][string]$Path)
  return [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
}

function Test-TcChildPath {
  param(
    [Parameter(Mandatory = $true)][string]$Parent,
    [Parameter(Mandatory = $true)][string]$Child
  )
  $taskParent = (Get-TcFullPath $Parent) + [IO.Path]::DirectorySeparatorChar
  $taskChild = (Get-TcFullPath $Child) + [IO.Path]::DirectorySeparatorChar
  return $taskChild.StartsWith($taskParent, [StringComparison]::OrdinalIgnoreCase)
}

function New-TcGameSandbox {
  [CmdletBinding()]
  param(
    [Parameter(Mandatory = $true)][string]$Repository,
    [Parameter(Mandatory = $true)][string]$Sandbox,
    [Parameter(Mandatory = $true)][string]$Game,
    [string[]]$Mods = @(),
    [switch]$Recreate,
    [switch]$SkipApply
  )

  $taskRepository = Get-TcFullPath $Repository
  $taskSandbox = Get-TcFullPath $Sandbox
  $taskGame = Get-TcFullPath $Game
  $taskBuild = Join-Path $taskRepository 'build'
  New-Item -ItemType Directory -Force $taskBuild | Out-Null

  if (-not (Test-TcChildPath -Parent $taskBuild -Child $taskSandbox)) {
    throw "Sandbox must be a child of the repository build directory: $taskSandbox"
  }

  $taskDirectories = @('asset', 'campaign', 'translations')
  $taskFiles = @(
    'Turing Complete.exe',
    'compile.dll',
    'tc_game_engine.dll',
    'soft_oal.dll',
    'steam_api64.dll',
    'libgcc_s_seh-1.dll',
    'libwinpthread-1.dll'
  )
  foreach ($taskDirectory in $taskDirectories) {
    $taskSource = Join-Path $taskGame $taskDirectory
    if (!(Test-Path -LiteralPath $taskSource -PathType Container)) {
      throw "Game directory is missing '$taskDirectory': $taskGame"
    }
  }
  foreach ($taskFile in $taskFiles) {
    $taskSource = Join-Path $taskGame $taskFile
    if (!(Test-Path -LiteralPath $taskSource -PathType Leaf)) {
      throw "Game directory is missing '$taskFile': $taskGame"
    }
  }
  foreach ($taskRequired in @('dist\tc-loader.dll', 'dist\tcmod-cli.exe')) {
    if (!(Test-Path -LiteralPath (Join-Path $taskRepository $taskRequired) -PathType Leaf)) {
      throw "Missing build output '$taskRequired'; run build.ps1 first"
    }
  }
  foreach ($taskMod in $Mods) {
    $taskModPath = Join-Path $taskRepository ('dist\' + $taskMod + '.mod')
    if (!(Test-Path -LiteralPath $taskModPath -PathType Leaf)) {
      throw "Missing Mod package '$taskModPath'; run build.ps1 first"
    }
  }

  if ($Recreate -and (Test-Path -LiteralPath $taskSandbox)) {
    Remove-Item -LiteralPath $taskSandbox -Recurse -Force
  }

  $taskRoot = Join-Path $taskSandbox 'game'
  $taskHome = Join-Path $taskSandbox 'home'
  $taskOut = Join-Path $taskSandbox 'out'
  New-Item -ItemType Directory -Force $taskRoot, $taskHome, $taskOut | Out-Null

  foreach ($taskDirectory in $taskDirectories) {
    $taskDestination = Join-Path $taskRoot $taskDirectory
    if (!(Test-Path -LiteralPath $taskDestination)) {
      Copy-Item -LiteralPath (Join-Path $taskGame $taskDirectory) -Destination $taskRoot -Recurse
    }
  }
  foreach ($taskFile in $taskFiles) {
    Copy-Item -LiteralPath (Join-Path $taskGame $taskFile) -Destination $taskRoot -Force
  }

  $taskModsPath = Join-Path $taskRoot 'mods'
  New-Item -ItemType Directory -Force $taskModsPath | Out-Null
  Copy-Item -LiteralPath (Join-Path $taskRepository 'dist\tc-loader.dll') `
    -Destination (Join-Path $taskRoot 'game_engine.dll') -Force
  Get-ChildItem -LiteralPath $taskModsPath -Filter '*.mod' -File | Remove-Item -Force
  foreach ($taskMod in $Mods) {
    Copy-Item -LiteralPath (Join-Path $taskRepository ('dist\' + $taskMod + '.mod')) `
      -Destination $taskModsPath -Force
  }

  if (!$SkipApply) {
    $taskApplyOutput = & (Join-Path $taskRepository 'dist\tcmod-cli.exe') $taskRoot apply @Mods
    if ($LASTEXITCODE) { throw "Sandbox Mod apply failed with exit code $LASTEXITCODE" }
    Write-Verbose ($taskApplyOutput -join [Environment]::NewLine)
  }

  return [pscustomobject]@{
    Sandbox = $taskSandbox
    Game = $taskRoot
    Home = $taskHome
    AppData = Join-Path $taskHome 'AppData\Roaming'
    Out = $taskOut
  }
}

function Invoke-TcIsolatedProfile {
  [CmdletBinding()]
  param(
    [Parameter(Mandatory = $true)][string]$Home,
    [Parameter(Mandatory = $true)][scriptblock]$ScriptBlock
  )
  $taskPreviousProfile = $env:USERPROFILE
  $taskPreviousAppData = $env:APPDATA
  try {
    $env:USERPROFILE = Get-TcFullPath $Home
    $env:APPDATA = Join-Path $env:USERPROFILE 'AppData\Roaming'
    New-Item -ItemType Directory -Force $env:APPDATA | Out-Null
    & $ScriptBlock
  } finally {
    $env:USERPROFILE = $taskPreviousProfile
    $env:APPDATA = $taskPreviousAppData
  }
}

function Stop-TcProcess {
  [CmdletBinding()]
  param(
    [Parameter(Mandatory = $true)]$Process,
    [int]$GraceMilliseconds = 4000
  )
  if ($Process -and !$Process.HasExited) {
    [void]$Process.CloseMainWindow()
    if (!$Process.WaitForExit($GraceMilliseconds)) {
      Stop-Process -Id $Process.Id -ErrorAction SilentlyContinue
    }
  }
}

function Wait-TcPath {
  [CmdletBinding()]
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][DateTime]$Deadline,
    $Process,
    [int]$PollMilliseconds = 250
  )
  while (!(Test-Path -LiteralPath $Path) -and [DateTime]::UtcNow -lt $Deadline) {
    if ($Process -and $Process.HasExited) { break }
    Start-Sleep -Milliseconds $PollMilliseconds
  }
  return Test-Path -LiteralPath $Path
}

Export-ModuleMember -Function New-TcGameSandbox, Invoke-TcIsolatedProfile, Stop-TcProcess, Wait-TcPath
