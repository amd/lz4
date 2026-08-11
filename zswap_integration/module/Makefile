# Copyright Advanced Micro Devices, Inc.
# SPDX-License-Identifier: BSD-3-Clause
# Convenience wrapper around the kernel Kbuild.
#
#   make            - build the full aocl_lz4.ko module, which registers the
#                     "aocl-lz4" compressor
#   make gluecheck  - compile ONLY the kernel-clean crypto+kernel_fpu glue TU
#                     (aocl_lz4_zswap.o) against the running kernel's headers.
#                     Proves the crypto scomp + kernel_fpu_begin/end integration
#                     is kernel-API-correct, independent of the AOCL core port.
#   make clean

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

gluecheck:
	$(MAKE) -C $(KDIR) M=$(PWD) aocl_lz4_zswap.o

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.o .*.o.cmd
