// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: BSD-3-Clause
/*
 * glue.c - kernel-API shim exporting the exact symbols zswap / the benchmark
 *          driver expect, on top of the adapted AOCL-LZ4 scalar core (lz4.c).
 *
 * The driver (harness/bench/zswap_bench.c) and the kernel crypto scomp path use:
 *     int LZ4_compress_default(const char *src, char *dst,
 *                              int srcSize, int dstCapacity, void *wrkmem);
 *     int LZ4_decompress_safe(const char *src, char *dst,
 *                             int compressedSize, int dstCapacity);
 *
 * - LZ4_decompress_safe is provided directly by lz4.c (AOCL's optimized
 *   decompress is AVX-only and is disabled here, so this is the upstream
 *   scalar decoder, == baseline behaviour). We do NOT redefine it.
 *
 * - LZ4_compress_default (5-arg, kernel signature) is defined here. AOCL's
 *   own public LZ4_compress_default is 4-arg (no wrkmem) and has been renamed
 *   in the adapted lz4.c to avoid the clash. This shim binds the optimized
 *   scalar internal DIRECTLY:
 *       AOCL_LZ4_compress_fast_extState_internal(wrkmem, src, dst,
 *                                                srcSize, dstCapacity, 1)
 *   with acceleration = 1. Calling the internal directly guarantees the
 *   scalar-optimized path without relying on runtime CPUID dispatch, and adds
 *   zero extra copies/allocs: the caller-provided wrkmem is used as the
 *   compression state (LZ4_initStream() runs over it inside the internal),
 *   exactly mirroring how the kernel passes per-CPU LZ4_MEM_COMPRESS scratch.
 */

/* Defined in lz4.c (non-static, external linkage), compiled with AOCL_LZ4_OPT.
 * Signature matches lz4.c:3243. Declared locally so this shim need not include
 * AOCL's lz4.h (whose 4-arg LZ4_compress_default prototype would otherwise
 * conflict with the 5-arg kernel symbol defined below). */
extern int AOCL_LZ4_compress_fast_extState_internal(void *state,
						    const char *source,
						    char *dest,
						    int inputSize,
						    int maxOutputSize,
						    int acceleration);

int LZ4_compress_default(const char *src, char *dst, int srcSize,
			 int dstCapacity, void *wrkmem)
{
	return AOCL_LZ4_compress_fast_extState_internal(wrkmem, src, dst,
							srcSize, dstCapacity, 1);
}
