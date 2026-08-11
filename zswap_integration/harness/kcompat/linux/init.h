// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: BSD-3-Clause
/* kcompat: userspace stand-in for <linux/init.h>. */
#ifndef _KCOMPAT_LINUX_INIT_H
#define _KCOMPAT_LINUX_INIT_H

#ifndef __init
#define __init
#endif
#ifndef __exit
#define __exit
#endif
#define module_init(fn)
#define module_exit(fn)

#endif /* _KCOMPAT_LINUX_INIT_H */
