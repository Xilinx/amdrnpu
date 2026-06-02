/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright(C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Linker fragment for OpenAMP resource-table placement (Phase 1
 * link A). Included via `zephyr_linker_sources(SECTIONS ...)` from
 * tests/tessai_agent_test/CMakeLists.txt - appended verbatim near
 * the end of the preprocessor-generated linker script.
 *
 * Places the .resource_table section emitted by
 * zephyr/lib/open-amp/resource_table.c into the RSC_TABLE memory-
 * region (apu_rsctbl @ 0x0A000000 per reserved.dtsi Section D), so
 * kernel-side remoteproc parsing of our loaded ELF finds the table
 * at the address it expects from the matching kernel-DT
 * rsc_tbl_carveout_1 entry.
 *
 * Same content/pattern as rpu-fw/rpu_fw/linker/resource_table.ld;
 * keep them in sync if either side restructures.
 */

#ifdef CONFIG_OPENAMP_RSC_TABLE
SECTION_PROLOGUE(.resource_table,, SUBALIGN(4))
{
	__resource_table_start = .;
	KEEP(*(.resource_table*))
	__resource_table_end = .;
} GROUP_LINK_IN(RSC_TABLE)
#endif
