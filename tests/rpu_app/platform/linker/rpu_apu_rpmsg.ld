/*
 * Copyright(C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom SOC linker script for RPURuntime on the tessai_rpu board with
 * link A (APU<->RPURuntime) OpenAMP slave-side rpmsg transport.
 *
 * Mirrors rpu-fw/rpu_fw/linker/rpu_openamp.ld but places the
 * .resource_table section in the RSC_TABLE memory region declared on
 * apu_rsctbl (OCM Bank 1, 0xBBF80000) in
 * RPURuntime/boards/amd/tessai_rpu/reserved.dtsi, so the Linux kernel
 * remoteproc framework can find the resource table when loading the
 * RPURuntime ELF. (Binding is by region NAME, so the placement follows
 * apu_rsctbl automatically wherever it is mapped.)
 *
 * Test apps that enable CONFIG_TESSAI_APU_RPMSG_SLAVE must point
 * SOC_LINKER_SCRIPT at this file before find_package(Zephyr).
 */

#include <zephyr/arch/arm/cortex_a_r/scripts/linker.ld>

SECTIONS
{
#ifdef CONFIG_OPENAMP_RSC_TABLE
	SECTION_PROLOGUE(.resource_table,, SUBALIGN(4))
	{
		__resource_table_start = .;
		KEEP(*(.resource_table*))
		__resource_table_end = .;
	} GROUP_LINK_IN(RSC_TABLE)
#endif
}
