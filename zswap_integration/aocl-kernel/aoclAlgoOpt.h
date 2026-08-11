// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: BSD-3-Clause
/*
 * aoclAlgoOpt.h - kernel-safe REPLACEMENT (NOT the cmake-generated upstream).
 *
 * Upstream algos/common/aoclAlgoOpt.h (generated from aoclAlgoOpt.h.in) sets
 * AOCL_MAX_ISA_LEVEL=4 by default and therefore *also* defines AOCL_LZ4_AVX_OPT
 * (and many other codec AVX flags). The AVX path pulls <immintrin.h> and
 * _mm256_* intrinsics, which are kernel-unsafe and forbidden by the build's
 * -mno-sse/-mno-avx flags.
 *
 * This minimal stand-in enables ONLY the scalar AOCL_LZ4_OPT path and never
 * defines AOCL_LZ4_AVX_OPT. It is included by lz4.h.
 */
#ifndef AOCL_KERNEL_ALGO_OPT_H
#define AOCL_KERNEL_ALGO_OPT_H

#ifndef AOCL_LZ4_OPT
#define AOCL_LZ4_OPT
#endif

#endif /* AOCL_KERNEL_ALGO_OPT_H */
