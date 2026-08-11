// Copyright Advanced Micro Devices, Inc.
// SPDX-License-Identifier: BSD-3-Clause
/* kcompat: userspace stand-in for <linux/module.h>.
 * EXPORT_SYMBOL / MODULE_* expand to harmless dummy declarations so the
 * unmodified kernel source compiles unchanged in userspace. */
#ifndef _KCOMPAT_LINUX_MODULE_H
#define _KCOMPAT_LINUX_MODULE_H

#include <linux/init.h>

#define EXPORT_SYMBOL(sym)      extern int __kcompat_export_##sym
#define EXPORT_SYMBOL_GPL(sym)  extern int __kcompat_export_gpl_##sym

#define MODULE_LICENSE(s)       extern int __kcompat_mod_license
#define MODULE_DESCRIPTION(s)   extern int __kcompat_mod_desc
#define MODULE_AUTHOR(s)        extern int __kcompat_mod_author
#define MODULE_VERSION(s)       extern int __kcompat_mod_version

#endif /* _KCOMPAT_LINUX_MODULE_H */
