/* SoftFloat build configuration for the float-ops Mod.

   Vendored SoftFloat 3e sources include "platform.h" as a quoted include, so
   the file has to sit on the include path of the build.  Apart from the
   THREAD_LOCAL line (see below) this is the configuration upstream ships for
   the same toolchain, `build/Win64-MinGW-w64/platform.h`; the four
   SOFTFLOAT_* options of the upstream Makefile are passed on the command line
   by examples/float-ops/build.ps1 and must match it:

       -DSOFTFLOAT_FAST_INT64 -DSOFTFLOAT_ROUND_ODD -DINLINE_LEVEL=5
       -DSOFTFLOAT_FAST_DIV32TO16 -DSOFTFLOAT_FAST_DIV64TO32

   THREAD_LOCAL is not defined upstream, which leaves SoftFloat's rounding
   mode, tininess mode and exception flags as plain globals shared by every
   thread.  The Mod runs the game's simulation thread and can be called from
   the UI thread while it refreshes a paused board, so the state has to be
   per-thread: with THREAD_LOCAL the declarations in softfloat.h and the
   definitions in softfloat_state.c agree on that storage class.  The wrapper
   still sets everything it depends on before every operation, so nothing
   leaks from one call to the next.

   This file is part of the SoftFloat IEEE Floating-Point Arithmetic Package,
   Release 3e, by John R. Hauser.

   Copyright 2011, 2012, 2013, 2014, 2015, 2016, 2017 The Regents of the
   University of California.  All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions, and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions, and the following disclaimer in the
       documentation and/or other materials provided with the distribution.

    3. Neither the name of the University nor the names of its contributors
       may be used to endorse or promote products derived from this software
       without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS "AS IS", AND ANY
   EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ARE
   DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR
   ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
   DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
   CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
   OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
   DAMAGE. */

#ifndef float_ops_softfloat_platform_h
#define float_ops_softfloat_platform_h

#define LITTLEENDIAN 1

#ifdef __GNUC_STDC_INLINE__
#define INLINE inline
#else
#define INLINE extern inline
#endif

/* Float Ops: SoftFloat's own state has to be per-thread (see the header
   comment).  softfloat.h and softfloat_state.c both use this macro, from C and
   from C++, so the keyword differs with the language; the header is also
   included by the kernel's C++ translation unit. */
#ifndef THREAD_LOCAL
#if defined(__cplusplus)
#define THREAD_LOCAL thread_local
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define THREAD_LOCAL _Thread_local
#else
#define THREAD_LOCAL
#endif
#endif

#define SOFTFLOAT_BUILTIN_CLZ 1
#define SOFTFLOAT_INTRINSIC_INT128 1
#include "opts-GCC.h"

#endif
