# The measurements simcore's semantics were written against, kept runnable so
# the claims in docs/PLAN-sandbox-simulator.md section 13.1 can be re-checked
# instead of trusted.  Each probe prints what it found; the VCD next to it has
# the transitions at full resolution.
#
# Read-only: nothing here touches the game.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path (Split-Path $PSScriptRoot)
$taskCompiler = if ($env:TC_MINGW_BIN) { $env:TC_MINGW_BIN } else { 'C:\msys64\ucrt64\bin' }
$taskIverilog = Join-Path $taskCompiler 'iverilog.exe'
$taskVvp = Join-Path $taskCompiler 'vvp.exe'
if (!(Test-Path -LiteralPath $taskIverilog) -or !(Test-Path -LiteralPath $taskVvp)) {
    "SKIP: Icarus Verilog is not installed in $taskCompiler"
    "      install it with: pacman -S --needed mingw-w64-ucrt-x86_64-iverilog"
    exit 0
}
$taskWork = Join-Path (Join-Path $taskRepo 'build') 'simcore-iv-probe'
New-Item -ItemType Directory -Force $taskWork | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'probes') -Destination $taskWork -Recurse -Force
Push-Location $taskWork
try {
    foreach ($taskName in @('probe1-initial-delay', 'probe2-edge-delays', 'probe3-resolution')) {
        "=== $taskName ==="
        & $taskIverilog -g2005 -o "$taskName.vvp" "probes\$taskName.v"
        if ($LASTEXITCODE) { throw "iverilog refused $taskName" }
        & $taskVvp "$taskName.vvp"
        if ($LASTEXITCODE) { throw "vvp failed on $taskName" }
    }
} finally {
    Pop-Location
}
"VCD files kept in $taskWork"
