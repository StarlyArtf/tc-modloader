# Berkeley SoftFloat 3e (vendored subset)

Upstream: <https://github.com/ucb-bar/berkeley-softfloat-3>

Commit: `a0c6494cdc11865811dec815d5c0049fba9d82a8`
(2025-03-07, "Merge pull request #34 from HarryR/allow-RISCV-CC-override")

License: see `COPYING.txt` (BSD 3-clause, retained verbatim).

## What is here

Only the sources the float-ops binary32 kernel actually calls, plus the headers
they include and one platform specialization:

```text
source/include/                       upstream headers (softfloat.h, internals.h,
                                      primitives.h, primitiveTypes.h,
                                      softfloat_types.h, opts-GCC.h)
source/*.c                            the 17 binary32 files listed in
                                      ../../fp/softfloat-sources.txt
source/ARM-VFPv2-defaultNaN/          specialize.h + the two files reachable
                                      from the kernel
COPYING.txt                           upstream license
```

Nothing in this tree is modified: the files are byte-identical to the upstream
commit above. The build configuration (include paths, `SOFTFLOAT_*` option
defines and SoftFloat's `platform.h`) lives in `../../fp/softfloat/platform.h`
and in `../../build.ps1`, so a future re-vendor is a plain file copy.

## Why the default-NaN specialization

`ARM-VFPv2-defaultNaN` is the specialization whose `defaultNaNF32UI` is
`0x7FC00000` and whose `softfloat_propagateNaNF32UI` always returns that
canonical quiet NaN (raising invalid only for signaling inputs) - exactly the
NaN policy `docs/PLAN-float-components.md` section 2.4 fixes for the Mod. The
`8086` specialization is not an option: its default NaN is the negative
`0xFFC00000` and it propagates payloads.

That specialization also sets `init_detectTininess` to
`softfloat_tininess_beforeRounding`, while the Mod's contract is
tininess-after-rounding. `fp32.cpp` therefore assigns
`softfloat_detectTininess` on every operation instead of relying on the
specialization's default.

## How the file list was computed

The list in `../../fp/softfloat-sources.txt` was not written by hand. Every
`.c` file of the upstream tree was compiled once with the same options the
build uses, `nm` listed each object's defined and undefined symbols, and the
reference graph was walked from the entry points the kernel calls (`f32_add`,
`f32_sub`, `f32_mul`, `f32_div`, `f32_sqrt`, `f32_mulAdd`, `f32_rem`,
`f32_roundToInt`, `softfloat_raiseFlags` and the three SoftFloat state
variables). The result is 19 compilation units with no unresolved symbol;
re-running the walk after a re-vendor is how the list stays honest.

SoftFloat's comparison entry points are deliberately *not* in that set: the
kernel implements the project's quiet relation, its classification and its
2019 min/max itself (plan 2.5), so `f32_eq`, `f32_lt`, `f32_le` and the
signaling variants would be dead weight in the Mod.

## The release-verification build is separate

Plan 10.1 asks for a TestFloat qualification of the shipped kernel. TestFloat
builds the *whole* upstream library from its own checkout with the same
specialization; it never links this subset. The subset only has to satisfy the
Mod's own closure, and `fp/softfloat-sources.txt` is the file that has to stay
in step with `fp32.cpp` if that changes.
