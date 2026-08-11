// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: BSD-3-Clause
/*
 * utils/utils.h - kernel-safe stub of AOCL's logging/timer/critical-section
 * utility header.
 *
 * The real utils/utils.h is NOT kernel-safe: in the (no-threads, C) build it
 * unconditionally pulls <immintrin.h> (for _mm_pause() inside the atomic
 * spin-lock critical section), <stdio.h>/<unistd.h>, and declares C++/unit-test
 * helpers. Under the build's -mno-sse/-mno-avx flags <immintrin.h> fails.
 *
 * lz4.c only needs the following from utils.h, all reduced to kernel-safe
 * no-ops / trivial substitutes here:
 *   - log-level tokens ERR/INFO/DEBUG/TRACE and LOG_UNFORMATTED/LOG_FORMATTED
 *     (logging disabled -> no fprintf, args not evaluated),
 *   - AOCL_SIMD_UNIT_TEST (unit-test hook -> no-op),
 *   - AOCL_ENTER_CRITICAL/AOCL_EXIT_CRITICAL (single-threaded build -> no-op,
 *     so no _mm_pause()/<immintrin.h> and no atomic spin),
 *   - get_disable_opt_flags() (env read -> always 0, i.e. "do not disable opt").
 *
 * <stdatomic.h> is included because the unmodified lz4.c declares
 *   static atomic_flag setup_lz4 = ATOMIC_FLAG_INIT;
 * (used only by the now-no-op critical section). <stdatomic.h> is a C11
 * freestanding header and does not require SIMD.
 */
#ifndef AOCL_KERNEL_UTILS_H
#define AOCL_KERNEL_UTILS_H

#include <stddef.h>
#include <stdatomic.h>

#if defined(__GNUC__) && __GNUC__ >= 3
#define FUNC_NAME __func__
#else
#define FUNC_NAME __FUNCTION__
#endif

/* Logging disabled: levels collapse to 0, log macros expand to nothing.
 * Because the macro bodies are empty, their arguments (including any
 * fprintf/printf-style format strings) are never evaluated or emitted. */
#define ERR    0
#define INFO   0
#define DEBUG  0
#define TRACE  0

#define LOG_UNFORMATTED(logType, logCtx, str)
#define LOG_FORMATTED(logType, logCtx, str, ...)

/* Unit-test SIMD-access hook: not built here. */
#define AOCL_SIMD_UNIT_TEST(logType, logCtx, str)

/* Single-threaded, kernel-safe build: the dispatcher-setup critical section
 * needs no real lock. ENTER/EXIT become no-ops (the upstream versions spin on
 * an atomic_flag using _mm_pause() from <immintrin.h>). */
#define AOCL_ENTER_CRITICAL(flag)
#define AOCL_EXIT_CRITICAL(flag)

/* AOCL_DISABLE_OPT env flag: always "off" (0) so optimized scalar path stays
 * selected. Real impl reads getenv("AOCL_DISABLE_OPT"). */
static inline ptrdiff_t get_disable_opt_flags(int printDebugLogs)
{
    (void)printDebugLogs;
    return 0;
}

#endif /* AOCL_KERNEL_UTILS_H */
