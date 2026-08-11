// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: BSD-3-Clause
/* kcompat: userspace stand-in for <linux/kernel.h>. */
#ifndef _KCOMPAT_LINUX_KERNEL_H
#define _KCOMPAT_LINUX_KERNEL_H

#include <linux/types.h>
#include <linux/bitops.h>

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

/* Compile-time assert; lz4defs.h maps LZ4_STATIC_ASSERT to this. */
#ifndef BUILD_BUG_ON
#define BUILD_BUG_ON(cond) ((void)sizeof(char[1 - 2 * !!(cond)]))
#endif

#endif /* _KCOMPAT_LINUX_KERNEL_H */
