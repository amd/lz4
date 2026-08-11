// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: BSD-3-Clause
/* kcompat: userspace stand-in for <linux/bitops.h>.
 * Only the bit primitives used by lib/lz4 are provided. __ffs/__fls match the
 * kernel semantics (0-indexed; undefined for 0, which lz4 never passes). */
#ifndef _KCOMPAT_LINUX_BITOPS_H
#define _KCOMPAT_LINUX_BITOPS_H

#include <stddef.h>

#ifndef BITS_PER_LONG
#define BITS_PER_LONG (sizeof(long) * 8)
#endif

#ifndef likely
#define likely(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/* find-first-set: index of least-significant set bit (0-based). */
static inline unsigned long __ffs(unsigned long word)
{
	return (unsigned long)__builtin_ctzl(word);
}

/* find-last-set: index of most-significant set bit (0-based). */
static inline unsigned long __fls(unsigned long word)
{
	return (unsigned long)(BITS_PER_LONG - 1 - __builtin_clzl(word));
}

#endif /* _KCOMPAT_LINUX_BITOPS_H */
