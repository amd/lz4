// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: BSD-3-Clause
/* kcompat: userspace stand-in for <asm/unaligned.h>.
 * Portable unaligned access via __builtin_memcpy (the kernel's safe method).
 * LE16 helpers are byte-assembled so they are endian-correct everywhere. */
#ifndef _KCOMPAT_ASM_UNALIGNED_H
#define _KCOMPAT_ASM_UNALIGNED_H

#include <stdint.h>

#define get_unaligned(ptr)                                                  \
	__extension__({                                                     \
		__typeof__(*(ptr)) __kc_v;                                  \
		__builtin_memcpy((void *)&__kc_v, (const void *)(ptr),      \
				 sizeof(__kc_v));                           \
		__kc_v;                                                     \
	})

#define put_unaligned(val, ptr)                                             \
	__extension__({                                                     \
		__typeof__(*(ptr)) __kc_v = (val);                          \
		__builtin_memcpy((void *)(ptr), (const void *)&__kc_v,      \
				 sizeof(__kc_v));                           \
	})

static inline uint16_t get_unaligned_le16(const void *p)
{
	const uint8_t *b = (const uint8_t *)p;
	return (uint16_t)(b[0] | (b[1] << 8));
}

static inline void put_unaligned_le16(uint16_t v, void *p)
{
	uint8_t *b = (uint8_t *)p;
	b[0] = (uint8_t)(v & 0xff);
	b[1] = (uint8_t)((v >> 8) & 0xff);
}

#endif /* _KCOMPAT_ASM_UNALIGNED_H */
