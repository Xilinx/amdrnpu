# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
# Portions of this file consist of AI-generated content.
#
# Convenience targets for editor indexes (repository root).

TAGS ?= tags
FILELIST ?= cscope.files
CTAGS ?= ctags
CSCOPE ?= cscope

# Prune tooling / build subtrees only; omit generated kernel *.mod.c.
FIND_PRUNE = \( \
	-path './.git' -o \
	-path './out' -o \
	-path './build' -o \
	-path './build-*' -o \
	-name 'oe-workdir' -o \
	-name 'oe-logs' -o \
	-name CMakeFiles -o \
	-name '.tmp_versions' \
\) -prune -o \
\( \( -name '*.c' -o -name '*.h' \) -type f \
	! -name '*.mod.c' ! -name '*.mod.h' -print \)

.PHONY: cscope.files tags cscope indexes clean-tags tags-cscope help

help:
	@echo "Indexing (.c /.h under this tree; requires ctags & cscope in PATH,"
	@echo "  or set CTAGS / CSCOPE to the binaries you use):"
	@echo "  make indexes       # cscope.files, tags, and cscope.out"
	@echo "  make cscope.files  # regenerate file list shared by targets below"
	@echo "  make tags          # exuberant/universal ctags \`tags\` file"
	@echo "  make cscope        # cscope -b -q inverted index databases"
	@echo "  make clean-tags    # remove generated index artifacts"

cscope.files:
	@rm -f '$(FILELIST).tmp'
	find . $(FIND_PRUNE) | LC_ALL=C sort -u > '$(FILELIST).tmp'
	mv '$(FILELIST).tmp' '$(FILELIST)'

tags: cscope.files
	$(CTAGS) -L '$(FILELIST)' -f '$(TAGS)'

cscope: cscope.files
	$(CSCOPE) -b -q -i '$(FILELIST)'

indexes: tags cscope

clean-tags:
	rm -f '$(TAGS)' TAGS '$(FILELIST)' cscope.out cscope.in.out cscope.po.out cscope.fast*.tbl

# Alias for workflow muscle-memory.
tags-cscope: indexes
