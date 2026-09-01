/* mlx_probe.c — diagnostic tool for debugging MlxRDMA dext initialization
 * WITHOUT rebuilding the driver (notes/35).
 *
 * Unlike mlx_reinit (which only does ReinitFw), mlx_probe lets you:
 * - run stages step by step: FLR → Exec → QueryPages → ProvidePages
 * - choose the mode: separate 4KiB buffers or a single 128KiB chunk (Apple-style)
 * - dump the full driver state
 *
 * The card owner does not change → IOPCIFamily does NOT do its FLR →
 * the card does not go into a long reset.
 *
 * Build:
 *   cc -o build/mlx_probe tools/mlx_probe.c -framework IOKit -framework CoreFoundation
 *
 * Usage:
 *   ./build/mlx_probe                    # full cycle: QueryPages → GIVE contig → DumpState
 *   ./build/mlx_probe --reinit           # ReinitFw (self-FLR + FwInit)
 *   ./build/mlx_probe --flr              # FLR only
 *   ./build/mlx_probe --query-pages      # QUERY_PAGES(BOOT)
 *   ./build/mlx_probe --give-sep         # GIVE with separate 4KiB buffers (old mode)
 *   ./build/mlx_probe --give-single      # GIVE with a single 128KiB chunk (Apple-style)
 *   ./build/mlx_probe --dump             # dump state
 *   ./build/mlx_probe --exec 0x107       # raw exec of an opcode
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include "MlxServiceMatch.h"

/* Must match Sources/userclient/MlxUCIO.h */
#define MLX_UC_SELECTOR_FWREINIT         0x10A0
#define MLX_UC_SELECTOR_DBG_FLR          0x10A1
#define MLX_UC_SELECTOR_DBG_EXEC         0x10A2
#define MLX_UC_SELECTOR_DBG_QUERY_PAGES  0x10A3
#define MLX_UC_SELECTOR_DBG_PROVIDE_PAGES 0x10A4
#define MLX_UC_SELECTOR_DBG_DUMP_STATE   0x10A5

/* Structs from MlxUCIO.h (only the ones needed by the tool) */
struct mlx_dbg_exec_req {
    uint32_t  opcode;
    uint32_t  inSize;
    uint32_t  outSize;
    uint32_t  timeoutMs;
    uint8_t   in[64];
};
struct mlx_dbg_exec_resp {
    uint32_t  kr;
    uint32_t  outSize;
    uint8_t   out[128];
};
struct mlx_dbg_query_pages_req {
    uint32_t  mode;
};
struct mlx_dbg_query_pages_resp {
    uint32_t  numPages;
    uint32_t  functionId;
    uint32_t  kr;
    uint32_t  rsvd;
};
struct mlx_dbg_provide_pages_req {
    uint32_t  numPages;
    uint32_t  mode;       /* 0=separate 1=chunk */
    uint32_t  ownership;
    uint32_t  rsvd;
};
struct mlx_dbg_provide_pages_resp {
    uint32_t  given;
    uint32_t  kr;
    uint64_t  iova[16];
};
struct mlx_dbg_state_resp {
    uint32_t  fwRev;
    uint32_t  cmdifRev;
    uint32_t  initializing;
    uint32_t  cmdqLogSzStride;
    uint64_t  cmdqIOVA;
    uint32_t  issi;
    uint32_t  hcaEnabled;
    uint32_t  pagesInUse;
    uint32_t  chunkMode;
    uint64_t  chunkIOVA;
    uint64_t  pageIOVA[8];
};

/* Opens a connection to the published MlxPCIDriver. */
static io_connect_t open_driver(void)
{
    io_service_t svc = IOServiceGetMatchingService(
        kIOMainPortDefault, mlxCreateServiceMatching());
    if (!svc) {
        fprintf(stderr, "MlxPCIDriver not found — card is not with our dext\n");
        return IO_OBJECT_NULL;
    }

    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "IOServiceOpen: 0x%x\n", kr);
        return IO_OBJECT_NULL;
    }
    printf("MlxPCIDriver found, UserClient opened\n");
    return conn;
}

/* ---- helpers ---- */

static void print_hex(const uint8_t *buf, uint32_t len, const char *label)
{
    printf("%s [%u bytes]:\n", label, len);
    for (uint32_t r = 0; r < len; r += 16) {
        uint32_t n = (len - r < 16) ? len - r : 16;
        printf("  ");
        for (uint32_t i = 0; i < n; i++)
            printf("%02x%c", buf[r+i], (i == 7) ? ' ' : ' ');
        printf("\n");
    }
}

static const char *kr_str(kern_return_t kr)
{
    switch (kr) {
    case kIOReturnSuccess: return "SUCCESS";
    case kIOReturnError: return "ERROR";
    case kIOReturnNoMemory: return "NO_MEMORY";
    case kIOReturnNoResources: return "NO_RESOURCES";
    case kIOReturnNotReady: return "NOT_READY";
    case kIOReturnIOError: return "IO_ERROR";
    case kIOReturnNotPermitted: return "NOT_PERMITTED";
    case kIOReturnUnsupported: return "UNSUPPORTED";
    case kIOReturnBusy: return "BUSY";
    case kIOReturnTimeout: return "TIMEOUT";
    case kIOReturnBadArgument: return "BAD_ARGUMENT";
    case kIOReturnNoDevice: return "NO_DEVICE";
    default: {
        static char buf[32];
        snprintf(buf, sizeof(buf), "0x%x", kr);
        return buf;
    }
    }
}

/* ---- operations ---- */

static int cmd_flr(io_connect_t conn)
{
    printf("--- FLR ---\n");
    kern_return_t kr = IOConnectCallMethod(conn, MLX_UC_SELECTOR_DBG_FLR,
        NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
    printf("FLR: %s\n", kr_str(kr));
    return (kr == kIOReturnSuccess) ? 0 : 1;
}

static int cmd_reinit(io_connect_t conn)
{
    printf("--- ReinitFw (self-FLR + FwInit, ~60s) ---\n");
    kern_return_t kr = IOConnectCallMethod(conn, MLX_UC_SELECTOR_FWREINIT,
        NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
    printf("ReinitFw: %s\n", kr_str(kr));
    return (kr == kIOReturnSuccess) ? 0 : 1;
}

static int cmd_query_pages(io_connect_t conn, uint32_t mode)
{
    printf("--- QUERY_PAGES(mode=%u) ---\n", mode);
    struct mlx_dbg_query_pages_req req = { .mode = mode };
    struct mlx_dbg_query_pages_resp resp = {};
    size_t outSize = sizeof(resp);
    kern_return_t kr = IOConnectCallMethod(conn, MLX_UC_SELECTOR_DBG_QUERY_PAGES,
        NULL, 0, &req, sizeof(req), NULL, NULL, &resp, &outSize);
    if (kr != kIOReturnSuccess) {
        printf("QUERY_PAGES: call failed: %s\n", kr_str(kr));
        return 1;
    }
    printf("QUERY_PAGES: kr=%s numPages=%u functionId=%u\n",
           kr_str(resp.kr), resp.numPages, resp.functionId);
    return (resp.kr == kIOReturnSuccess) ? 0 : 1;
}

static int cmd_provide_pages(io_connect_t conn, uint32_t numPages, uint32_t mode)
{
    const char *modeStr = (mode == 1) ? "CONTIG 128KiB" : "SEPARATE 4KiB";
    printf("--- MANAGE_PAGES(GIVE) %u pages, mode=%s ---\n", numPages, modeStr);
    struct mlx_dbg_provide_pages_req req = {
        .numPages = numPages,
        .mode = mode,
        .ownership = 1,  /* boot */
    };
    struct mlx_dbg_provide_pages_resp resp = {};
    size_t outSize = sizeof(resp);
    kern_return_t kr = IOConnectCallMethod(conn, MLX_UC_SELECTOR_DBG_PROVIDE_PAGES,
        NULL, 0, &req, sizeof(req), NULL, NULL, &resp, &outSize);
    if (kr != kIOReturnSuccess) {
        printf("PROVIDE_PAGES: call failed: %s\n", kr_str(kr));
        return 1;
    }
    printf("PROVIDE_PAGES: kr=%s given=%u\n", kr_str(resp.kr), resp.given);
    for (uint32_t i = 0; i < resp.given && i < 16; i++)
        printf("  iova[%u] = 0x%llx\n", i, (unsigned long long)resp.iova[i]);
    return (resp.kr == kIOReturnSuccess) ? 0 : 1;
}

static int cmd_dump(io_connect_t conn)
{
    printf("--- DumpState ---\n");
    struct mlx_dbg_state_resp resp = {};
    size_t outSize = sizeof(resp);
    kern_return_t kr = IOConnectCallMethod(conn, MLX_UC_SELECTOR_DBG_DUMP_STATE,
        NULL, 0, NULL, 0, NULL, NULL, &resp, &outSize);
    if (kr != kIOReturnSuccess) {
        printf("DumpState: call failed: %s\n", kr_str(kr));
        return 1;
    }
    printf("fw_rev        = 0x%08x\n", resp.fwRev);
    printf("cmdif_rev     = %u\n", resp.cmdifRev);
    printf("initializing  = 0x%08x (bit31=%u)\n", resp.initializing,
           (resp.initializing >> 31) & 1);
    printf("cmdq log_sz   = %u, stride = %u\n",
           (resp.cmdqLogSzStride >> 4) & 0xF, resp.cmdqLogSzStride & 0xF);
    printf("cmdq IOVA     = 0x%llx\n", (unsigned long long)resp.cmdqIOVA);
    printf("ISSI          = %u\n", resp.issi);
    printf("hcaEnabled    = %u\n", resp.hcaEnabled);
    printf("pagesInUse    = %u\n", resp.pagesInUse);
    printf("chunkMode     = %u\n", resp.chunkMode);
    printf("chunkIOVA     = 0x%llx\n", (unsigned long long)resp.chunkIOVA);
    for (uint32_t i = 0; i < 8 && resp.pageIOVA[i]; i++)
        printf("  page[%u] IOVA = 0x%llx\n", i, (unsigned long long)resp.pageIOVA[i]);
    return 0;
}

static int cmd_exec(io_connect_t conn, uint32_t opcode)
{
    printf("--- Exec opcode=0x%04x ---\n", opcode);
    struct mlx_dbg_exec_req req = {
        .opcode = opcode,
        .inSize = 16,
        .outSize = 16,
        .timeoutMs = 5000,
    };
    /* Build minimal input: opcode at bytes 0-1 (MSB-first as mlx5_ifc). */
    req.in[0] = (uint8_t)(opcode >> 8);
    req.in[1] = (uint8_t)(opcode & 0xFF);
    struct mlx_dbg_exec_resp resp = {};
    size_t outSize = sizeof(resp);
    kern_return_t kr = IOConnectCallMethod(conn, MLX_UC_SELECTOR_DBG_EXEC,
        NULL, 0, &req, sizeof(req), NULL, NULL, &resp, &outSize);
    if (kr != kIOReturnSuccess) {
        printf("Exec: call failed: %s\n", kr_str(kr));
        return 1;
    }
    printf("Exec: kr=%s outSize=%u\n", kr_str(resp.kr), resp.outSize);
    print_hex(resp.out, resp.outSize < 32 ? resp.outSize : 32, "out");
    return (resp.kr == kIOReturnSuccess) ? 0 : 1;
}

/* ---- full cycle ---- */
static int full_cycle(io_connect_t conn)
{
    int rc = 0;

    /* 1. QUERY_PAGES(BOOT) */
    printf("\n=== Step 1: QUERY_PAGES(BOOT) ===\n");
    struct mlx_dbg_query_pages_req qreq = { .mode = 1 };
    struct mlx_dbg_query_pages_resp qresp = {};
    size_t qoutSize = sizeof(qresp);
    kern_return_t kr = IOConnectCallMethod(conn, MLX_UC_SELECTOR_DBG_QUERY_PAGES,
        NULL, 0, &qreq, sizeof(qreq), NULL, NULL, &qresp, &qoutSize);
    if (kr != kIOReturnSuccess) { printf("QUERY_PAGES call failed\n"); return 1; }
    printf("numPages=%u functionId=%u kr=%s\n",
           qresp.numPages, qresp.functionId, kr_str(qresp.kr));
    if (qresp.kr != kIOReturnSuccess) { rc = 1; }

    /* 2. GIVE (contig mode) */
    printf("\n=== Step 2: GIVE %u pages (contig 128KiB) ===\n", qresp.numPages);
    if (qresp.numPages > 0) {
        struct mlx_dbg_provide_pages_req preq = {
            .numPages = qresp.numPages,
            .mode = 1,  /* contig */
            .ownership = 1,
        };
        struct mlx_dbg_provide_pages_resp presp = {};
        size_t poutSize = sizeof(presp);
        kr = IOConnectCallMethod(conn, MLX_UC_SELECTOR_DBG_PROVIDE_PAGES,
            NULL, 0, &preq, sizeof(preq), NULL, NULL, &presp, &poutSize);
        if (kr != kIOReturnSuccess) { printf("GIVE call failed\n"); rc = 1; }
        else {
            printf("GIVE: kr=%s given=%u\n", kr_str(presp.kr), presp.given);
            for (uint32_t i = 0; i < presp.given && i < 16; i++)
                printf("  iova[%u] = 0x%llx\n", i, (unsigned long long)presp.iova[i]);
            if (presp.kr != kIOReturnSuccess) rc = 1;
        }
    }

    /* 3. DumpState */
    printf("\n=== Step 3: DumpState ===\n");
    cmd_dump(conn);

    return rc;
}

/* ---- main ---- */
int main(int argc, char **argv)
{
    if (argc == 1) {
        /* No arguments — full cycle. */
        io_connect_t conn = open_driver();
        if (!conn) return 1;
        int rc = full_cycle(conn);
        IOServiceClose(conn);
        return rc;
    }

    /* With arguments — the chosen operation. */
    io_connect_t conn = open_driver();
    if (!conn) return 1;

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--reinit") == 0) {
            rc |= cmd_reinit(conn);
        } else if (strcmp(argv[i], "--flr") == 0) {
            rc |= cmd_flr(conn);
        } else if (strcmp(argv[i], "--query-pages") == 0) {
            rc |= cmd_query_pages(conn, 1);
        } else if (strcmp(argv[i], "--give-sep") == 0) {
            rc |= cmd_query_pages(conn, 1);
            rc |= cmd_provide_pages(conn, 6, 0);
        } else if (strcmp(argv[i], "--give-single") == 0) {
            rc |= cmd_query_pages(conn, 1);
            rc |= cmd_provide_pages(conn, 6, 1);
        } else if (strcmp(argv[i], "--dump") == 0) {
            rc |= cmd_dump(conn);
        } else if (strcmp(argv[i], "--exec") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--exec requires an opcode (hex)\n");
                rc = 1;
            } else {
                uint32_t op = (uint32_t)strtoul(argv[++i], NULL, 16);
                rc |= cmd_exec(conn, op);
            }
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            fprintf(stderr, "Usage:\n");
            fprintf(stderr, "  %s                    — full cycle\n", argv[0]);
            fprintf(stderr, "  %s --reinit           — ReinitFw\n", argv[0]);
            fprintf(stderr, "  %s --flr              — FLR only\n", argv[0]);
            fprintf(stderr, "  %s --query-pages      — QUERY_PAGES(BOOT)\n", argv[0]);
            fprintf(stderr, "  %s --give-sep         — GIVE separate 4KiB buffers\n", argv[0]);
            fprintf(stderr, "  %s --give-single      — GIVE single 128KiB chunk\n", argv[0]);
            fprintf(stderr, "  %s --dump             — dump state\n", argv[0]);
            fprintf(stderr, "  %s --exec 0x107       — raw exec\n", argv[0]);
            rc = 1;
        }
    }

    IOServiceClose(conn);
    return rc;
}
