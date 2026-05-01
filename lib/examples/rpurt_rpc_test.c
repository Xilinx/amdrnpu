/* SPDX-License-Identifier: MIT */
/*
 * rpurt_rpc_test - apu_aie_test-equivalent CLI built on rpurt_rpc lib.
 *
 * Mirrors the subcommand UX of rpu-fw/tools/apu_aie_test/apu_aie_test
 * so we can run head-to-head against the firmware reference binary on
 * the same board.  Each subcommand prints a short "Sending ..." banner
 * and a one-line result, matching the high-level flavour of the
 * reference output (the per-message hex dumps that apu_aie_test prints
 * are skipped: this CLI talks through the lib, which already validates
 * the wire layout for us).
 *
 * Usage:
 *   rpurt_rpc_test <command> [args...]
 *   rpurt_rpc_test --device /dev/rpurt_rpc.0 <command> [args...]
 *
 * Commands:
 *   echo <val1> <val2>
 *   identify
 *   tile_info
 *   partition create <col_start> <col_count>
 *   partition destroy <id>
 *   hwctx create <part_id> <num_tiles> [priority]
 *   hwctx destroy <id> [graceful]
 *   job submit <ctx_id> <job_type> <exec_time_ms>
 *   job status <id>
 *   job wait <id> [timeout_sec=30]
 *   job list
 *   lifecycle <col_start> <col_count> <num_tiles> <job_type> <exec_ms>
 *             [priority=0] [timeout_sec=30]
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpurt_rpc/rpurt_rpc.h"

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static uint32_t parse_u32(const char *s)
{
    return (uint32_t)strtoul(s, NULL, 0);
}

static const char *job_state_str(enum rpurt_rpc_aie_job_state st)
{
    switch (st) {
    case RPURT_RPC_AIE_JOB_FREE:      return "FREE";
    case RPURT_RPC_AIE_JOB_PENDING:   return "PENDING";
    case RPURT_RPC_AIE_JOB_RUNNING:   return "RUNNING";
    case RPURT_RPC_AIE_JOB_COMPLETED: return "COMPLETED";
    case RPURT_RPC_AIE_JOB_FAILED:    return "FAILED";
    default:                          return "UNKNOWN";
    }
}

static int report(const char *what, int rc)
{
    if (rc == 0)
        return 0;
    fprintf(stderr, "%s: %s (rc=%d)\n", what, strerror(-rc), rc);
    return 1;
}

/* ------------------------------------------------------------------ */
/* per-command handlers                                                */
/* ------------------------------------------------------------------ */

static int do_echo(rpurt_rpc_handle *h, int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: echo <val1> <val2>\n");
        return 2;
    }
    uint32_t v1 = parse_u32(argv[0]);
    uint32_t v2 = parse_u32(argv[1]);
    printf("Sending AIE ECHO: val1=0x%08x, val2=0x%08x\n", v1, v2);
    int rc = rpurt_rpc_aie_echo(h, v1, v2);
    if (rc)
        return report("echo", rc);
    printf("ECHO test PASSED\n");
    return 0;
}

static int do_identify(rpurt_rpc_handle *h)
{
    struct rpurt_rpc_aie_fw_version v;
    printf("Sending IDENTIFY\n");
    int rc = rpurt_rpc_aie_identify(h, &v);
    if (rc)
        return report("identify", rc);
    printf("Firmware version: %u.%u.%u (build %u)\n",
           v.major, v.minor, v.patch, v.build);
    return 0;
}

static int do_tile_info(rpurt_rpc_handle *h)
{
    struct rpurt_rpc_aie_tile_geometry g;
    printf("Requesting AIE tile info\n");
    int rc = rpurt_rpc_aie_tile_info(h, &g);
    if (rc)
        return report("tile_info", rc);
    printf("AIE Tile Info:\n");
    printf("  Version: %u.%u\n", g.major, g.minor);
    printf("  Columns: %u, Rows: %u\n", g.cols, g.rows);
    printf("  Core rows: %u (start: %u)\n", g.core_rows, g.core_row_start);
    printf("  Mem rows: %u (start: %u)\n",  g.mem_rows,  g.mem_row_start);
    printf("  Shim rows: %u (start: %u)\n", g.shim_rows, g.shim_row_start);
    printf("  DMA channels - core: %u, mem: %u, shim: %u\n",
           g.core_dma_channels, g.mem_dma_channels, g.shim_dma_channels);
    printf("  Locks - core: %u, mem: %u, shim: %u\n",
           g.core_locks, g.mem_locks, g.shim_locks);
    return 0;
}

static int do_partition(rpurt_rpc_handle *h, int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "Usage: partition create|destroy ...\n");
        return 2;
    }
    if (strcmp(argv[0], "create") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: partition create <col_start> <col_count>\n");
            return 2;
        }
        uint32_t col_start = parse_u32(argv[1]);
        uint32_t col_count = parse_u32(argv[2]);
        uint32_t id = 0;
        printf("Creating partition: col_start=%u, col_count=%u\n",
               col_start, col_count);
        int rc = rpurt_rpc_aie_partition_create(h, col_start, col_count, &id);
        if (rc)
            return report("partition create", rc);
        printf("Response: status=0, partition_id=%u\n", id);
        return 0;
    }
    if (strcmp(argv[0], "destroy") == 0) {
        if (argc < 2) {
            fprintf(stderr, "Usage: partition destroy <partition_id>\n");
            return 2;
        }
        uint32_t id = parse_u32(argv[1]);
        printf("Destroying partition: partition_id=%u\n", id);
        int rc = rpurt_rpc_aie_partition_destroy(h, id);
        if (rc)
            return report("partition destroy", rc);
        printf("Response: status=0\n");
        return 0;
    }
    fprintf(stderr, "Unknown partition command: %s\n", argv[0]);
    return 2;
}

static int do_hwctx(rpurt_rpc_handle *h, int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "Usage: hwctx create|destroy ...\n");
        return 2;
    }
    if (strcmp(argv[0], "create") == 0) {
        if (argc < 3) {
            fprintf(stderr,
                    "Usage: hwctx create <partition_id> <num_tiles> [priority]\n");
            return 2;
        }
        uint32_t part_id   = parse_u32(argv[1]);
        uint32_t num_tiles = parse_u32(argv[2]);
        uint32_t priority  = (argc > 3) ? parse_u32(argv[3]) : 0;
        uint32_t ctx_id = 0, doorbell = 0, msix = 0;
        printf("Creating HW context: partition=%u, tiles=%u, priority=%u\n",
               part_id, num_tiles, priority);
        int rc = rpurt_rpc_aie_hwctx_create(h, part_id, num_tiles, priority,
                                            &ctx_id, &doorbell, &msix);
        if (rc)
            return report("hwctx create", rc);
        printf("HW Context created:\n");
        printf("  context_id: %u\n", ctx_id);
        printf("  doorbell_offset: 0x%x\n", doorbell);
        printf("  msix_idx: %u\n", msix);
        return 0;
    }
    if (strcmp(argv[0], "destroy") == 0) {
        if (argc < 2) {
            fprintf(stderr,
                    "Usage: hwctx destroy <context_id> [graceful=1]\n");
            return 2;
        }
        uint32_t ctx_id   = parse_u32(argv[1]);
        int      graceful = (argc > 2) ? atoi(argv[2]) : 1;
        printf("Destroying HW context: id=%u, graceful=%d\n", ctx_id, graceful);
        int rc = rpurt_rpc_aie_hwctx_destroy(h, ctx_id, graceful);
        if (rc)
            return report("hwctx destroy", rc);
        printf("HW Context %u destroyed\n", ctx_id);
        return 0;
    }
    fprintf(stderr, "Unknown hwctx command: %s\n", argv[0]);
    return 2;
}

static int do_job(rpurt_rpc_handle *h, int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "Usage: job submit|status|wait ...\n");
        return 2;
    }
    if (strcmp(argv[0], "submit") == 0) {
        if (argc < 4) {
            fprintf(stderr,
                    "Usage: job submit <context_id> <job_type> <exec_time_ms>\n");
            return 2;
        }
        uint32_t ctx  = parse_u32(argv[1]);
        uint32_t type = parse_u32(argv[2]);
        uint32_t ms   = parse_u32(argv[3]);
        uint32_t job_id = 0;
        printf("Submitting job: context=%u, type=%u, exec_time=%u ms\n",
               ctx, type, ms);
        int rc = rpurt_rpc_aie_job_submit(h, ctx, type, ms, &job_id);
        if (rc)
            return report("job submit", rc);
        printf("Job submitted: job_id=%u\n", job_id);
        return 0;
    }
    if (strcmp(argv[0], "status") == 0) {
        if (argc < 2) {
            fprintf(stderr, "Usage: job status <job_id>\n");
            return 2;
        }
        uint32_t job_id = parse_u32(argv[1]);
        enum rpurt_rpc_aie_job_state st = RPURT_RPC_AIE_JOB_FREE;
        int32_t result = 0;
        printf("Querying job status: id=%u\n", job_id);
        int rc = rpurt_rpc_aie_job_status(h, job_id, &st, &result);
        if (rc)
            return report("job status", rc);
        printf("Job %u: state=%s, result=%d\n",
               job_id, job_state_str(st), result);
        return 0;
    }
    if (strcmp(argv[0], "wait") == 0) {
        if (argc < 2) {
            fprintf(stderr, "Usage: job wait <job_id> [timeout_sec=30]\n");
            return 2;
        }
        uint32_t job_id      = parse_u32(argv[1]);
        unsigned int timeout = (argc > 2) ? (unsigned int)atoi(argv[2]) : 30u;
        int32_t result = 0;
        printf("Waiting for job %u (timeout=%us)\n", job_id, timeout);
        int rc = rpurt_rpc_aie_job_wait(h, job_id, timeout * 1000u, &result);
        if (rc == 0) {
            printf("Job %u completed: result=%d\n", job_id, result);
            return 0;
        }
        if (rc == -EIO) {
            printf("Job %u failed: result=%d\n", job_id, result);
            return 1;
        }
        if (rc == -ETIMEDOUT) {
            printf("Timeout waiting for job %u\n", job_id);
            return 1;
        }
        return report("job wait", rc);
    }
    if (strcmp(argv[0], "list") == 0) {
        struct rpurt_rpc_aie_job_summary s;
        printf("Listing jobs\n");
        int rc = rpurt_rpc_aie_job_list(h, &s);
        if (rc)
            return report("job list", rc);
        printf("Job Summary:\n");
        printf("  Total:     %u\n", s.total);
        printf("  Pending:   %u\n", s.pending);
        printf("  Running:   %u\n", s.running);
        printf("  Completed: %u\n", s.completed);
        return 0;
    }
    fprintf(stderr, "Unknown job command: %s\n", argv[0]);
    return 2;
}

/*
 * Composite end-to-end exercise: partition_create -> hwctx_create ->
 * job_submit -> job_wait -> hwctx_destroy -> partition_destroy. Used
 * to smoke-test the full M2 opcode chain in a single invocation. Best
 * effort cleanup: any resources acquired before a failure are torn
 * down on the way out so we don't leak partitions/contexts on the
 * firmware side between runs.
 */
static int do_lifecycle(rpurt_rpc_handle *h, int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: lifecycle <col_start> <col_count> <num_tiles> "
                "<job_type> <exec_ms> [priority=0] [timeout_sec=30]\n");
        return 2;
    }
    uint32_t col_start = parse_u32(argv[0]);
    uint32_t col_count = parse_u32(argv[1]);
    uint32_t num_tiles = parse_u32(argv[2]);
    uint32_t job_type  = parse_u32(argv[3]);
    uint32_t exec_ms   = parse_u32(argv[4]);
    uint32_t priority  = (argc > 5) ? parse_u32(argv[5]) : 0u;
    unsigned int timeout_sec = (argc > 6) ? (unsigned int)atoi(argv[6]) : 30u;

    uint32_t part_id = 0, ctx_id = 0, doorbell = 0, msix = 0, job_id = 0;
    int32_t  job_result = 0;
    int      have_part = 0, have_ctx = 0;
    int      rc;

    printf("=== lifecycle: col=[%u..%u) tiles=%u job_type=%u "
           "exec_ms=%u priority=%u timeout=%us ===\n",
           col_start, col_start + col_count, num_tiles,
           job_type, exec_ms, priority, timeout_sec);

    printf("[1/6] partition_create(col_start=%u, col_count=%u)\n",
           col_start, col_count);
    rc = rpurt_rpc_aie_partition_create(h, col_start, col_count, &part_id);
    if (rc) { rc = report("partition create", rc); goto out; }
    have_part = 1;
    printf("      -> partition_id=%u\n", part_id);

    printf("[2/6] hwctx_create(partition=%u, tiles=%u, priority=%u)\n",
           part_id, num_tiles, priority);
    rc = rpurt_rpc_aie_hwctx_create(h, part_id, num_tiles, priority,
                                    &ctx_id, &doorbell, &msix);
    if (rc) { rc = report("hwctx create", rc); goto out; }
    have_ctx = 1;
    printf("      -> ctx_id=%u doorbell=0x%x msix=%u\n",
           ctx_id, doorbell, msix);

    printf("[3/6] job_submit(ctx=%u, type=%u, exec_ms=%u)\n",
           ctx_id, job_type, exec_ms);
    rc = rpurt_rpc_aie_job_submit(h, ctx_id, job_type, exec_ms, &job_id);
    if (rc) { rc = report("job submit", rc); goto out; }
    printf("      -> job_id=%u\n", job_id);

    printf("[4/6] job_wait(job=%u, timeout=%us)\n", job_id, timeout_sec);
    rc = rpurt_rpc_aie_job_wait(h, job_id, timeout_sec * 1000u, &job_result);
    if (rc == 0) {
        printf("      -> COMPLETED, result=%d\n", job_result);
    } else if (rc == -EIO) {
        printf("      -> FAILED, result=%d\n", job_result);
        rc = 1;
    } else if (rc == -ETIMEDOUT) {
        printf("      -> TIMEOUT after %us\n", timeout_sec);
        rc = 1;
    } else {
        rc = report("job wait", rc);
    }
    /* Fall through to cleanup regardless of job outcome. */

out:
    if (have_ctx) {
        printf("[5/6] hwctx_destroy(ctx=%u, graceful=1)\n", ctx_id);
        int rc2 = rpurt_rpc_aie_hwctx_destroy(h, ctx_id, 1);
        if (rc2)
            (void)report("hwctx destroy (cleanup)", rc2);
        else
            printf("      -> ok\n");
    }
    if (have_part) {
        printf("[6/6] partition_destroy(partition=%u)\n", part_id);
        int rc2 = rpurt_rpc_aie_partition_destroy(h, part_id);
        if (rc2)
            (void)report("partition destroy (cleanup)", rc2);
        else
            printf("      -> ok\n");
    }
    if (rc == 0)
        printf("=== lifecycle: PASSED ===\n");
    else
        printf("=== lifecycle: FAILED (rc=%d) ===\n", rc);
    return rc;
}

/* ------------------------------------------------------------------ */
/* dispatch                                                            */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [--device /dev/rpurt_rpc.N] <command> [args...]\n"
        "\n"
        "Commands:\n"
        "  echo <val1> <val2>\n"
        "  identify\n"
        "  tile_info\n"
        "  partition create <col_start> <col_count>\n"
        "  partition destroy <id>\n"
        "  hwctx create <part_id> <num_tiles> [priority]\n"
        "  hwctx destroy <id> [graceful]\n"
        "  job submit <ctx_id> <job_type> <exec_time_ms>\n"
        "  job status <id>\n"
        "  job wait <id> [timeout_sec=30]\n"
        "  job list\n"
        "  lifecycle <col_start> <col_count> <num_tiles> <job_type> "
        "<exec_ms> [priority=0] [timeout_sec=30]\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *path = RPURT_RPC_DEVICE_PATH;
    int idx = 1;

    /* Accept --help / -h / help in any position before the subcommand. */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-h")     == 0 ||
            strcmp(argv[i], "help")   == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    if (idx < argc && strcmp(argv[idx], "--device") == 0) {
        if (idx + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        path = argv[idx + 1];
        idx += 2;
    }

    if (idx >= argc) {
        usage(argv[0]);
        return 2;
    }

    rpurt_rpc_handle *h = rpurt_rpc_open_path(path);
    if (!h) {
        fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
        return 1;
    }

    const char *cmd = argv[idx];
    char      **rest = argv + idx + 1;
    int         nrest = argc - idx - 1;
    int         rc;

    if      (strcmp(cmd, "echo")      == 0) rc = do_echo     (h, nrest, rest);
    else if (strcmp(cmd, "identify")  == 0) rc = do_identify (h);
    else if (strcmp(cmd, "tile_info") == 0) rc = do_tile_info(h);
    else if (strcmp(cmd, "partition") == 0) rc = do_partition(h, nrest, rest);
    else if (strcmp(cmd, "hwctx")     == 0) rc = do_hwctx    (h, nrest, rest);
    else if (strcmp(cmd, "job")       == 0) rc = do_job      (h, nrest, rest);
    else if (strcmp(cmd, "lifecycle") == 0) rc = do_lifecycle(h, nrest, rest);
    else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        usage(argv[0]);
        rc = 2;
    }

    rpurt_rpc_close(h);
    return rc;
}
