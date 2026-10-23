# SPDX-License-Identifier: MIT
# x86_64 host (driver side) of the XRT/AIE offload over the MDB5 PCIe-DMA
# rpmsg-lite path. Native build; links the rpmsg_lite_dma library + MDB5 backend.
#
# Build on the x86 TARGET:
#   make
#
# Layout-flexible: by default it finds the rpmsg-lite sources, apu_ops.h and
# amdrnpu_wire.h in the amdrnpu tree. Override the locations if you copied only
# a subset to the target:
#   make RPMSG=/path/to/lib/src/rpmsg-lite LIBSRC=/path/to/lib/src EXAMPLES=/path/to/lib/examples
#
# NOTE: CC/CFLAGS use ':=' on purpose. This dir is often used with the aarch64
# EDF SDK env sourced (it exports CC/CFLAGS to cross-build the RPU side); the
# host must stay native x86_64. A makefile ':=' overrides the environment,
# while an explicit 'make CC=...' still wins.

CC     := gcc
CFLAGS := -O2 -g -std=c11 -Wall -Wextra
CFLAGS += $(EXTRA_CFLAGS)

# Auto-locate the amdrnpu tree pieces (override with make RPMSG=... etc.).
RPMSG_CANDIDATES := ../../../lib/src/rpmsg-lite ./rpmsg-lite ../rpmsg-lite .
RPMSG ?= $(firstword $(foreach d,$(RPMSG_CANDIDATES),\
                     $(if $(wildcard $(d)/rpmsg_lite_dma.c),$(d))))

LIBSRC_CANDIDATES := ../../../lib/src ../lib/src .
LIBSRC ?= $(firstword $(foreach d,$(LIBSRC_CANDIDATES),\
                      $(if $(wildcard $(d)/amdrnpu_wire.h),$(d))))

EXAMPLES_CANDIDATES := ../../../lib/examples ../lib/examples .
EXAMPLES ?= $(firstword $(foreach d,$(EXAMPLES_CANDIDATES),\
                        $(if $(wildcard $(d)/apu_ops.h),$(d))))

CPPFLAGS := -I. -I$(RPMSG) -I$(LIBSRC) -I$(EXAMPLES)

# Shared rpmsg-lite + MDB5 transport, linked into every host binary.
TRANSPORT := $(RPMSG)/rpmsg_lite_dma.c \
             $(RPMSG)/vring_lite_dma.c \
             $(RPMSG)/rpmsg_mem_mdb5.c

LDLIBS := -lz

# Generic descriptor-driven host + ResNet50-specialised host (IFM packing,
# fixed karg layout, OFM readback). Both reuse the same transport.
BINS := xrt_offload_host resnet_offload_host

all: $(BINS)

ifeq ($(RPMSG),)
$(error could not find rpmsg-lite sources; set RPMSG=/path/to/rpmsg-lite)
endif
ifeq ($(LIBSRC),)
$(error could not find amdrnpu_wire.h; set LIBSRC=/path/to/lib/src)
endif
ifeq ($(EXAMPLES),)
$(error could not find apu_ops.h; set EXAMPLES=/path/to/lib/examples)
endif

xrt_offload_host: xrt_offload_host.c $(TRANSPORT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

resnet_offload_host: resnet_offload_host.c $(TRANSPORT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -f $(BINS)

.PHONY: all clean
