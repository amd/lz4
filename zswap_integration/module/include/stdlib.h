/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal kernel-build shim for GCC intrinsic headers.
 *
 * <immintrin.h> includes GCC's mm_malloc.h, and mm_malloc.h includes
 * <stdlib.h> for size_t, NULL, malloc() and free(). Kbuild uses -nostdinc,
 * so libc's stdlib.h is unavailable. The AOCL-LZ4 AVX decoder never calls
 * _mm_malloc/_mm_free, but the inline definitions must still parse.
 */
#ifndef AOCL_LZ4_KERNEL_STDLIB_SHIM_H
#define AOCL_LZ4_KERNEL_STDLIB_SHIM_H

#include <linux/types.h>
#include <linux/slab.h>

/*
 * mm_malloc.h expects <stdlib.h> to provide NULL. Under -nostdinc it is not
 * guaranteed to be defined yet, so provide it here if needed.
 */
#ifndef NULL
#define NULL ((void *)0)
#endif

static inline void *malloc(size_t size)
{
	return kmalloc(size, GFP_KERNEL);
}

static inline void free(void *ptr)
{
	kfree(ptr);
}

#endif /* AOCL_LZ4_KERNEL_STDLIB_SHIM_H */
