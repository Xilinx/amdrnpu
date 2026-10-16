# SPDX-License-Identifier: MIT
# x86_64 host (driver side) of the rpmsg-lite DMA echo test.
# Native build; links the vetted rpmsg_lite_dma library + MDB5 backend.
#
# Build on the rave2 x86 TARGET:
#   make
#
# Layout-flexible: by default it finds the rpmsg-lite sources and echo_config.c
# in the amdrnpu tree. If you copied only a subset to the target, either keep
# the relative layout, drop the rpmsg-lite *.c/*.h and echo_config.* next to
# this Makefile, or point at them explicitly:
#   make RPMSG=/path/to/lib/src/rpmsg-lite CFGDIR=/path/to/echo_config_dir
#
# NOTE: CC/CFLAGS use ':=' (not '?=') on purpose. This dir is often used with
# the aarch64 EDF SDK env sourced (it exports CC/CFLAGS to cross-build the A78
# side); the host must stay native x86_64. A makefile ':=' overrides the
# environment, while an explicit 'make CC=...' still wins.

CC     := gcc
CFLAGS := -O2 -g -std=c11 -Wall -Wextra
CFLAGS += $(EXTRA_CFLAGS)

# Auto-locate the rpmsg-lite sources (override with 'make RPMSG=...').
RPMSG_CANDIDATES := ../../../lib/src/rpmsg-lite ./rpmsg-lite ../rpmsg-lite .
RPMSG ?= $(firstword $(foreach d,$(RPMSG_CANDIDATES),\
                     $(if $(wildcard $(d)/rpmsg_lite_dma.c),$(d))))

# Auto-locate echo_config.c (override with 'make CFGDIR=...').
CFG_CANDIDATES := .. .
CFGDIR ?= $(firstword $(foreach d,$(CFG_CANDIDATES),\
                      $(if $(wildcard $(d)/echo_config.c),$(d))))

CPPFLAGS := -I. -I$(CFGDIR) -I$(RPMSG)

SRCS := echo_host.c \
        $(CFGDIR)/echo_config.c \
        $(RPMSG)/rpmsg_lite_dma.c \
        $(RPMSG)/vring_lite_dma.c \
        $(RPMSG)/rpmsg_mem_mdb5.c

BIN := echo_host

all: $(BIN)

$(BIN): $(SRCS)
ifeq ($(RPMSG),)
	$(error could not find rpmsg-lite sources; set RPMSG=/path/to/rpmsg-lite)
endif
ifeq ($(CFGDIR),)
	$(error could not find echo_config.c; set CFGDIR=/path/to/dir)
endif
	$(CC) $(CFLAGS) $(CPPFLAGS) $(SRCS) -o $@

clean:
	rm -f $(BIN)

.PHONY: all clean
