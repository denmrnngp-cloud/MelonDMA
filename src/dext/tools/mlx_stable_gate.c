/* Stable-driver firmware gate.
 *
 * Exercises Linux's complete function close/open sequence without FLR:
 * TEARDOWN_HCA -> TAKE pages -> DISABLE_HCA -> ENABLE_HCA -> startup pages
 * -> SET_HCA_CAP -> INIT_HCA.  The DEXT returns a complete report even when
 * it auto-recovers through FLR after a failed experimental cycle.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include "MlxServiceMatch.h"
#include "MlxUCIO.h"

static io_connect_t open_driver(void)
{
    io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, mlxCreateServiceMatching());
    if (!service) {
        fprintf(stderr, "STABLE_DRIVER FAIL: MlxPCIDriver service not found\n");
        return IO_OBJECT_NULL;
    }
    io_connect_t connection = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connection);
    IOObjectRelease(service);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "STABLE_DRIVER FAIL: IOServiceOpen 0x%x\n", kr);
        return IO_OBJECT_NULL;
    }
    return connection;
}

static int query_port(io_connect_t connection)
{
    struct mlx_query_port_resp port = {0};
    size_t output_size = sizeof(port);
    kern_return_t kr = IOConnectCallStructMethod(
        connection, kMlxUCMethodQueryPort, NULL, 0, &port, &output_size);
    if (kr != kIOReturnSuccess || output_size != sizeof(port) ||
        port.portState != 1) {
        fprintf(stderr,
                "STABLE_DRIVER FAIL: port readback kr=0x%x size=%zu state=%u\n",
                kr, output_size, port.portState);
        return 1;
    }
    return 0;
}

static const char *failure_stage_name(uint32_t stage)
{
    static const char *const names[] = {
        "none", "teardown", "event-drain", "reclaim", "disable",
        "enable", "issi", "boot-query", "boot-give", "set-cap",
        "init-query", "init-give", "init-hca", "query-cap", "phase2",
        "verify"
    };
    return stage < sizeof(names) / sizeof(names[0]) ? names[stage] : "unknown";
}

static const char *phase2_substage_name(uint32_t sub)
{
    static const char *const names[] = {
        "none", "dma-init", "uar-init", "alloc-pd", "alloc-xrcd",
        "alloc-uar", "eq-init", "create-eq", "roce-init", "enable-vport",
        "query-port", "create-cq", "create-qp", "modify-qp",
        "destroy-qp", "destroy-cq", "health-init", "eq-poller"
    };
    return sub < sizeof(names) / sizeof(names[0]) ? names[sub] : "unknown";
}

int main(int argc, char **argv)
{
    unsigned cycles = 2;
    int require_take = 1;
    int preflight_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cycles") && i + 1 < argc) {
            char *end = NULL;
            errno = 0;
            unsigned long parsed = strtoul(argv[++i], &end, 10);
            if (errno || !end || *end || parsed < 1 || parsed > 100) {
                fprintf(stderr, "invalid --cycles value\n");
                return 2;
            }
            cycles = (unsigned)parsed;
        } else if (!strcmp(argv[i], "--allow-no-negative-take")) {
            /* Compatibility spelling retained for older automation. */
            require_take = 0;
        } else if (!strcmp(argv[i], "--preflight")) {
            preflight_only = 1;
        } else {
            fprintf(stderr,
                    "usage: %s [--cycles 1..100] [--allow-no-negative-take] [--preflight]\n",
                    argv[0]);
            return 2;
        }
    }

    io_connect_t connection = open_driver();
    if (!connection) return 1;
    if (query_port(connection)) {
        IOServiceClose(connection);
        return 1;
    }
    if (preflight_only) {
        printf("STABLE_DRIVER_PREFLIGHT PASS: user client opened, port UP\n");
        IOServiceClose(connection);
        return 0;
    }

    uint64_t total_event_requests = 0;
    uint64_t total_event_pages = 0;
    uint64_t total_event_returned = 0;
    uint64_t total_reclaim_requested = 0;
    uint64_t total_reclaim_returned = 0;
    for (unsigned i = 0; i < cycles; i++) {
        struct mlx_stable_init_cycle_resp report = {0};
        size_t output_size = sizeof(report);
        kern_return_t call_kr = IOConnectCallStructMethod(
            connection, kMlxUCMethodStableInitCycle,
            NULL, 0, &report, &output_size);
        printf("cycle=%u call=0x%x result=0x%x stage=%u(%s) "
               "opcode=0x%04x delivery=%u fw_status=%u syndrome=0x%08x "
               "teardown=%u init=%u phase2=%u "
               "substage=%u(%s) phase2_ret=0x%x "
               "fw=%08x/%08x cmdq=%016" PRIx64 "/%016" PRIx64 " "
               "owner_cap=%u owner=%08x:%08x:%08x:%08x "
               "firmware_negative=%u/%u/%u explicit_reclaim=%u/%u "
               "pages=%u/%u ambiguous=%u "
               "accounting=%u recovered_flr=%u\n",
               report.cycle, call_kr, report.kr, report.failureStage,
               failure_stage_name(report.failureStage),
               report.lastOpcode, report.lastDeliveryStatus,
               report.lastFwStatus, report.lastSyndrome, report.teardownOk,
               report.initOk, report.phase2Ok,
               report.phase2SubStage, phase2_substage_name(report.phase2SubStage),
               report.phase2Ret,
               report.fwRevBefore,
               report.fwRevAfter, report.cmdqIOVABefore,
               report.cmdqIOVAAfter, report.swOwnerIdSupported,
               report.swOwnerId[0], report.swOwnerId[1],
               report.swOwnerId[2], report.swOwnerId[3],
               report.negativeTakeRequests, report.negativeTakePages,
               report.negativeTakeReturned, report.reclaimRequested,
               report.reclaimReturned, report.fwOwnedBefore,
               report.fwOwnedAfter, report.ambiguousAfter,
               report.accountingOk, report.recoveredWithFlr);

        int owner_ok = !report.swOwnerIdSupported ||
            (report.swOwnerId[0] | report.swOwnerId[1] |
             report.swOwnerId[2] | report.swOwnerId[3]);
        if (call_kr != kIOReturnSuccess || output_size != sizeof(report) ||
            report.kr != kIOReturnSuccess || !report.teardownOk ||
            !report.initOk || !report.phase2Ok ||
            report.failureStage != MLX_STABLE_STAGE_NONE ||
            report.fwRevBefore != report.fwRevAfter ||
            report.cmdqIOVABefore != report.cmdqIOVAAfter ||
            !owner_ok || report.ambiguousAfter || !report.accountingOk ||
            report.recoveredWithFlr || query_port(connection)) {
            fprintf(stderr, "STABLE_DRIVER FAIL: no-FLR INIT cycle %u\n", i + 1);
            IOServiceClose(connection);
            return 1;
        }
        uint64_t cycle_requested = (uint64_t)report.negativeTakePages +
                                   report.reclaimRequested;
        uint64_t cycle_returned = (uint64_t)report.negativeTakeReturned +
                                  report.reclaimReturned;
        if (require_take &&
            (!cycle_requested || cycle_requested != cycle_returned ||
             cycle_requested != report.fwOwnedBefore)) {
            fprintf(stderr,
                    "STABLE_DRIVER FAIL: cycle %u TAKE coverage "
                    "requested=%" PRIu64 " returned=%" PRIu64
                    " fw_owned_before=%u\n",
                    i + 1, cycle_requested, cycle_returned,
                    report.fwOwnedBefore);
            IOServiceClose(connection);
            return 1;
        }
        total_event_requests += report.negativeTakeRequests;
        total_event_pages += report.negativeTakePages;
        total_event_returned += report.negativeTakeReturned;
        total_reclaim_requested += report.reclaimRequested;
        total_reclaim_returned += report.reclaimReturned;
    }
    IOServiceClose(connection);

    uint64_t total_requested = total_event_pages + total_reclaim_requested;
    uint64_t total_returned = total_event_returned + total_reclaim_returned;
    if (require_take && (!total_requested || total_requested != total_returned)) {
        fprintf(stderr,
                "STABLE_DRIVER FAIL: TAKE not fully completed "
                "(requested=%" PRIu64 " returned=%" PRIu64 ")\n",
                total_requested, total_returned);
        return 1;
    }

    printf("STABLE_DRIVER PASS: %u no-FLR INIT_HCA cycles; "
           "firmware_negative=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "; "
           "explicit_reclaim=%" PRIu64 "/%" PRIu64 "; "
           "total_TAKE=%" PRIu64 "/%" PRIu64 "\n",
           cycles, total_event_requests, total_event_pages,
           total_event_returned, total_reclaim_requested,
           total_reclaim_returned, total_requested, total_returned);
    return 0;
}
