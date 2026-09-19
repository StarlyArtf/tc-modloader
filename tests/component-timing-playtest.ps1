# Actual game compilation and menu score regressions, each in its own profile.
$ErrorActionPreference='Stop'
$taskRepo=Split-Path $PSScriptRoot
$taskKeys=@('TC_FIXTURE','TC_SOLUTION','TC_COST_MODES','TC_EXPECT_DELAY','TC_EXPECT_GATES','TC_REPORT','TC_EXPECT_UI','TC_LEVEL','TC_EXPECT_LOG','TC_TOPO','TC_DECLARE_GATES','TC_DECLARE_DELAY','TC_EXTRA_MODS')
$taskPrevious=@{}
foreach($taskKey in $taskKeys) { $taskPrevious[$taskKey]=[Environment]::GetEnvironmentVariable($taskKey) }
try {
  foreach($taskCase in @(
    @{Name='single5';Topology='single';Delay=5;Expected=5;Gates=1;Mode='plain'},
    @{Name='series5';Topology='series';Delay=5;Expected=10;Gates=2;Mode='plain'},
    @{Name='parallel5';Topology='parallel';Delay=5;Expected=5;Gates=2;Mode='plain'},
    @{Name='single0';Topology='single';Delay=0;Expected=0;Gates=1;Mode='plain'},
    @{Name='builtin';Topology='single';Delay=5;Expected=1;Gates=1;Mode='builtin'},
    @{Name='nested';Topology='nested';Delay=7;Expected=7;Gates=1;Mode='plain'},
    # Stateful design: the inner Delay Line (kind 0x0d) costs 5 gates / 4 delay
    # according to the game's own cost function, so the declared pair must match.
    @{Name='stateful';Topology='delay';Delay=4;Expected=4;Gates=5;Mode='plain';DesignGates=5},
    # Multiple drivers on one net keep the game's native timing and say so in
    # the loader log: the verified rules do not cover that shape.
    @{Name='multidriver';Topology='multidriver';Delay=5;Expected=1;Gates=2;Mode='plain';ExpectLog='native timing retained'},
    @{Name='sandbox';Topology='single';Delay=5;Expected=5;Gates=1;Mode='plain';Level='sandbox'},
    @{Name='foundry';Topology='single';Delay=5;Expected=5;Gates=1;Mode='plain';Level='foundry'}
  )) {
    $taskDir=Join-Path $taskRepo ('build\timing-'+$taskCase.Name)
    $env:TC_FIXTURE=Join-Path $taskDir 'and2_component.data'
    $taskDesignGates = if($taskCase.DesignGates) { $taskCase.DesignGates } else { 1 }
    & (Join-Path $taskRepo 'build\and-component-fixture.exe') $env:TC_FIXTURE $taskDesignGates $taskCase.Delay $taskCase.Topology
    if($LASTEXITCODE){throw 'Timing fixture generation failed'}
    $env:TC_SOLUTION=Join-Path $taskDir 'and2_solution.data'
    $env:TC_COST_MODES=$taskCase.Mode
    $env:TC_LEVEL=if($taskCase.Level) { $taskCase.Level } else { 'and_gate' }
    $env:TC_EXPECT_DELAY=[string]$taskCase.Expected
    $env:TC_EXPECT_GATES=[string]$taskCase.Gates
    $env:TC_EXPECT_UI='1'
    $env:TC_EXPECT_LOG=if($taskCase.ExpectLog) { $taskCase.ExpectLog } else { '' }
    $env:TC_REPORT=Join-Path $taskRepo ('build\timing-'+$taskCase.Name+'-report.txt')
    & (Join-Path $taskRepo 'tests\component-cost-playtest.ps1')
  }
} finally {
  foreach($taskKey in $taskKeys) { [Environment]::SetEnvironmentVariable($taskKey,$taskPrevious[$taskKey]) }
}
'PASS real-game component timing and displayed scores'
