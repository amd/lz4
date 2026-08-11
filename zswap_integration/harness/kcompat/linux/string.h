// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: BSD-3-Clause
/* kcompat: userspace stand-in for <linux/string.h>.
 * memcpy/memset/memmove are freestanding-required builtins; declare via libc. */
#ifndef _KCOMPAT_LINUX_STRING_H
#define _KCOMPAT_LINUX_STRING_H
#include <string.h>
#endif /* _KCOMPAT_LINUX_STRING_H */
