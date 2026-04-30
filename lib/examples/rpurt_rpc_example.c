/* SPDX-License-Identifier: MIT */
/*
 * rpurt_rpc_example - end-to-end AIE management ECHO round-trip.
 *
 * Opens /dev/rpurt_rpc (or the path given on the command line), queries
 * the driver ABI, then issues an AIE ECHO command to the bound RPU
 * firmware via rpurt_rpc_aie_echo() and reports whether the firmware
 * echoed val1 / val2 back unchanged.
 *
 * This replaces the placeholder "kfifo loopback" example that pre-dated
 * the rpmsg transport: with a real rpmsg channel, sending arbitrary
 * bytes that are not a valid npu_mbox message would just hang waiting
 * for a response that never comes.
 *
 * Usage:
 *   rpurt_rpc_example                               # default vals on /dev/rpurt_rpc
 *   rpurt_rpc_example /dev/rpurt_rpc.0
 *   rpurt_rpc_example /dev/rpurt_rpc.0 0x12345678 0xDEADBEEF
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpurt_rpc/rpurt_rpc.h"

static uint32_t parse_u32(const char *s, uint32_t fallback)
{
    if (!s || !*s)
        return fallback;
    return (uint32_t)strtoul(s, NULL, 0);
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : RPURT_RPC_DEVICE_PATH;
    uint32_t val1 = parse_u32(argc > 2 ? argv[2] : NULL, 0x12345678u);
    uint32_t val2 = parse_u32(argc > 3 ? argv[3] : NULL, 0xDEADBEEFu);

    rpurt_rpc_handle *h = rpurt_rpc_open_path(path);
    if (!h) {
        fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
        return 1;
    }

    struct rpurt_rpc_info info;
    int rc = rpurt_rpc_get_info(h, &info);
    if (rc) {
        fprintf(stderr, "get_info: %s\n", strerror(-rc));
        rpurt_rpc_close(h);
        return 1;
    }
    printf("rpurt-rpc lib %s, driver ABI v%u, max payload %u\n",
           rpurt_rpc_version(), info.abi_version, info.max_payload);

    printf("Sending AIE ECHO: val1=0x%08x, val2=0x%08x\n", val1, val2);
    rc = rpurt_rpc_aie_echo(h, val1, val2);
    if (rc) {
        fprintf(stderr, "AIE echo: %s (rc=%d)\n", strerror(-rc), rc);
        rpurt_rpc_close(h);
        return 1;
    }

    printf("ECHO test PASSED\n");
    rpurt_rpc_close(h);
    return 0;
}
