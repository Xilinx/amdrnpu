/* SPDX-License-Identifier: MIT */
/*
 * rpurt_rpc_example - minimal sanity check for the rpurt_rpc library.
 *
 * Opens the device, queries its ABI version, sends a small message and
 * tries to receive it back (the kernel placeholder just loops messages).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "rpurt_rpc/rpurt_rpc.h"

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : RPURT_RPC_DEVICE_PATH;
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

    const char *payload = "hello rpu";
    rc = rpurt_rpc_send(h, 0xC0FFEE, payload, strlen(payload) + 1);
    if (rc) {
        fprintf(stderr, "send: %s\n", strerror(-rc));
        rpurt_rpc_close(h);
        return 1;
    }

    struct rpurt_rpc_msg msg;
    rc = rpurt_rpc_recv(h, &msg);
    if (rc) {
        fprintf(stderr, "recv: %s\n", strerror(-rc));
        rpurt_rpc_close(h);
        return 1;
    }
    printf("recv opcode=0x%x len=%u data=\"%.*s\"\n",
           msg.opcode, msg.len, (int)msg.len, msg.data);

    rpurt_rpc_close(h);
    return 0;
}
