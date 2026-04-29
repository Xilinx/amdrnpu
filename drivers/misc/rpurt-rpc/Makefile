# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build wrapper for the rpurt-rpc kernel module.
#
# Usage:
#   make KDIR=/path/to/linux                              # native build
#   make KDIR=... ARCH=arm64 CROSS_COMPILE=aarch64-...-   # cross build
#   make KDIR=... modules_install INSTALL_MOD_PATH=/tmp/r # staged install
#   make clean
#
# KDIR can be set as an environment variable, on the make command line,
# or via the legacy KERNEL_SRC variable that Yocto's kernel.bbclass exports.

# Allow either KDIR or KERNEL_SRC (Yocto convention) to point at the
# kernel build tree. Fall back to the running kernel's build symlink.
KDIR       ?= $(KERNEL_SRC)
KDIR       ?= /lib/modules/$(shell uname -r)/build

# Directory of this Makefile (with trailing slash stripped).
PWD        := $(shell pwd)

# Cross-compile knobs are simply forwarded; the kernel build system
# honors ARCH and CROSS_COMPILE from the environment.

.PHONY: all modules modules_install clean help

all: modules

modules:
	@if [ -z "$(KDIR)" ] || [ ! -d "$(KDIR)" ]; then \
		echo "ERROR: KDIR is not set or does not exist: '$(KDIR)'"; \
		echo "       Pass KDIR=/path/to/linux on the command line."; \
		exit 1; \
	fi
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	rm -f *.o *.ko *.mod *.mod.c *.mod.o .*.cmd Module.symvers modules.order
	rm -rf .tmp_versions

help:
	@echo "rpurt-rpc kernel module - out-of-tree build"
	@echo ""
	@echo "Targets:"
	@echo "  all / modules     build rpurt_rpc.ko"
	@echo "  modules_install   install module to INSTALL_MOD_PATH (default /)"
	@echo "  clean             remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  KDIR              path to the kernel source/build tree (required)"
	@echo "  KERNEL_SRC        alternate spelling of KDIR (Yocto)"
	@echo "  ARCH              target architecture (e.g. arm64)"
	@echo "  CROSS_COMPILE     cross toolchain prefix"
	@echo "  INSTALL_MOD_PATH  staging root for modules_install"
