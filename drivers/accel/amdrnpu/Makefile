# SPDX-License-Identifier: GPL-2.0
#
# Copyright (C) 2026, Advanced Micro Devices, Inc.

KDIR       ?= $(KERNEL_SRC)
KDIR       ?= /lib/modules/$(shell uname -r)/build
PWD        := $(shell pwd)

ifneq ($(strip $(O)),)
O_MAKE := O=$(strip $(O))
else
O_MAKE :=
endif

.PHONY: all modules modules_install clean help

all: modules

modules:
	@if [ -z "$(KDIR)" ] || [ ! -d "$(KDIR)" ]; then \
		echo "ERROR: KDIR is not set or does not exist: '$(KDIR)'"; \
		echo "       Pass KDIR=/path/to/linux on the command line."; \
		exit 1; \
	fi
	+$(MAKE) $(O_MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	+$(MAKE) $(O_MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	+$(MAKE) $(O_MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	rm -f *.o *.ko *.mod *.mod.c *.mod.o .*.cmd Module.symvers modules.order
	rm -rf .tmp_versions

help:
	@echo "amdrnpu kernel module - out-of-tree build"
	@echo ""
	@echo "Targets:"
	@echo "  all / modules     build amdrnpu.ko"
	@echo "  modules_install   install module to INSTALL_MOD_PATH (default /)"
	@echo "  clean             remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  KDIR              kernel source/build tree (required)"
	@echo "  KERNEL_SRC        Yocto-style synonym of KDIR"
	@echo "  O                 kernel build/output dir when split from source (Yocto)"
	@echo "  ARCH, CROSS_COMPILE, INSTALL_MOD_PATH - forwarded as usual"
