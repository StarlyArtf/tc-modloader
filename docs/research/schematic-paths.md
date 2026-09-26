# Where a level's board lives on disk

Measured 2026-09-26 with the pinned build (2.1.334) while fixing the
"退出再进后常量回到默认值" report.  It answers one question the loader's
`TC_COMMAND_SAVE` had been guessing at: **which directory does the game read and
write a level's board from?**

## The measurement

Recipe (`build/float-branchprobe-*`, reproducible with a scratch copy of
`.research/branch-probe.ps1`):

1. a fresh profile whose `settings.txt` names a level (`setting_current_level`),
2. two *different* fixtures, one in each candidate directory:
   * `schematics/architecture/Default/circuit.data` ← the M2 board (4 components)
   * `schematics/architecture/sandbox/circuit.data` ← the catalogue board (19)
   also tried as `schematics/sandbox/Default/circuit.data`,
3. start the game, enter the level from the screen's own level block, and read the
   loader log: which board appears, and which file the game writes afterwards.

Result (twice, with `setting_current_level` = `architecture` and = `introduction`):

* the board that appears is the one in `schematics/architecture/Default/` - the
  level's **kind** directory, not its name and not the `setting_current_level`;
* the game *re-writes* that same file a second or two after loading it (the v13
  fixture came back as a 185-byte v16 file), which is the game's own save path;
* the other candidate directory was never read and never modified.

Why "architecture": the level "The Sandbox" is stored in the campaign under
`campaign/sandbox/`, and its `meta.txt` says

```text
kind = architecture
title = (31337_58382627590046, `The Sandbox`)
```

The loader logs the level as `Level loaded: sandbox` (its name) - which is what
`currentLevelName` holds and what `TC_COMMAND_SAVE` used to build its path from.

## What it cost

`src/native.hpp`'s `saveCurrentCircuit` wrote
`schematics/<level name>/Default/circuit.data`, so a Mod's configuration landed in
`schematics/sandbox/Default/circuit.data` while the game kept reading
`schematics/architecture/Default/circuit.data`.  Nothing failed, no error was
logged: the next visit simply rebuilt the board from a file that never saw the
edit, and a Constant came back with its default.

## The rule the loader follows now

`levelSchematicKind()` reads `<root>/campaign/<level name>/meta.txt`, takes its
`kind` (rejecting anything that is not a plain directory component), and
`saveCurrentCircuit` writes `schematics/<kind>/Default/circuit.data`.  A level
with no meta - anything this build does not ship - keeps the older name-based
behaviour and says so in the log:

```text
Circuit save: wrote current Board to …/schematics/architecture/Default/circuit.data
 (level "sandbox" keeps its board in the "architecture" schematic the level's own
  meta names as its kind)
```

The same rule is what the true-game cases rely on when they inject a board: a
fixture meant for the level `sandbox` goes to
`schematics/architecture/Default/circuit.data` (`tests/float-reentry-playtest.ps1`,
`tests/float-panel-playtest.ps1`).  Verified end to end by
`tests/float-reentry-playtest.ps1` (case id `float-reentry-game`): red on the
previous package, green with this rule.
