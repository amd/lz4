// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: BSD-3-Clause
/* kcompat: userspace stand-in for <linux/types.h>.
 * Provides the fixed-width integer types and the kernel inline attribute that
 * the lib/lz4 sources expect, without pulling in real kernel headers. */
#ifndef _KCOMPAT_LINUX_TYPES_H
#define _KCOMPAT_LINUX_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif
#ifndef noinline
#define noinline __attribute__((noinline))
#endif
#ifndef __maybe_unused
#define __maybe_unused __attribute__((unused))
#endif

#endif /* _KCOMPAT_LINUX_TYPES_H */
