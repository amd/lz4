// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: BSD-3-Clause
/*
 * utils/dispatcher.h - kernel-safe stub of AOCL's CPU dynamic-dispatch header.
 *
 * The real utils/dispatcher.h only DECLARES the Dispatcher_* APIs; the bodies
 * live in utils/dispatcher.cpp (C++, runtime CPUID, getenv). That is neither
 * freestanding nor kernel-safe. lz4.c needs three things from this header:
 *   - the CpuFeatures type and OptimizationLevel enum (used by the FMV tables
 *     in aocl_lz4_dispatch_variants.h / aocl_lz4_fmv_utils.h),
 *   - the Dispatcher_* entry points called from aocl_setup_lz4()/
 *     aocl_setup_native().
 *
 * Since the kernel-safe glue calls AOCL_LZ4_compress_fast_extState_internal
 * directly (no runtime dispatch), these stubs just report "no special CPU
 * features / scalar level", which keeps the default scalar function pointers.
 * They are static inline so no extra translation unit / symbol is needed.
 */
#ifndef DISPATCH_DISPATCHER_H
#define DISPATCH_DISPATCHER_H

#if defined(__KERNEL__)
#include <linux/types.h>
#else
#include <stdint.h>
#endif

typedef uint64_t CpuFeatures;

/* Feature bits kept for source compatibility with the FMV variant tables.
 * Only referenced inside AVX-gated code (compiled out here). */
#define FEATURE_SSE2       (1ULL << 0)
#define FEATURE_AVX        (1ULL << 1)
#define FEATURE_AVX2       (1ULL << 2)
#define FEATURE_AVX512F    (1ULL << 3)

typedef enum {
    OPTLEVEL_AUTO   = -1,
    OPTLEVEL_SCALAR = 0,
    OPTLEVEL_SSE2   = 1,
    OPTLEVEL_AVX    = 2,
    OPTLEVEL_AVX2   = 3,
    OPTLEVEL_AVX512 = 4
} OptimizationLevel;

static inline OptimizationLevel Dispatcher_IntToLevel(int level)
{
    (void)level;
    return OPTLEVEL_SCALAR;
}

static inline CpuFeatures Dispatcher_GetSupportedFeaturesForLevel(OptimizationLevel level)
{
    (void)level;
    return (CpuFeatures)0; /* no SIMD features advertised -> scalar path */
}

static inline OptimizationLevel Dispatcher_GetEnvRequestedLevel(void)
{
    return OPTLEVEL_SCALAR;
}

static inline CpuFeatures Dispatcher_GetFeaturesFromEnv(void)
{
    return (CpuFeatures)0;
}

#endif /* DISPATCH_DISPATCHER_H */
