/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Phase 1 link A acceptance: passive main.
 *
 * apu_rpmsg_platform.c (SYS_INIT APPLICATION) brings up the link A
 * OpenAMP slave transport against the apu_rsctbl / apu_rpmsg_shmem
 * carveouts and the ipi_2_to_ipi_3 mailbox, then
 * tessai_agent_endpoint.c (SYS_INIT APPLICATION +10) registers the
 * "tessai-agent" rpmsg endpoint and starts its RX worker.
 *
 * Once SYS_INIT finishes Zephyr enters main(); we have nothing to do
 * here beyond logging "ready" and parking on k_sleep so the kernel
 * keeps the endpoint workers and the link A RX thread alive. The APU
 * side drives all traffic.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(tessai_agent_test, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("Phase 1 RPURuntime up: link A tessai-agent endpoint ready");
	LOG_INF("Idling main(); SYS_INIT brought up apu_rpmsg + tessai endpoint");

	for (;;) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
