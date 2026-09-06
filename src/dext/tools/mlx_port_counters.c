/* mlx_port_counters — the card's own view of the wire.
 *
 * The DEXT owns the port and macOS has no netif behind it, so ifconfig and
 * netstat show nothing: this is the only way to see received packets, bytes,
 * discards and pause frames on the Mac side. Snapshot mode prints the raw
 * counters; --watch prints the delta over an interval, which is what a
 * transfer measurement needs.
 *
 * --pcie additionally dumps the PCIe link the card sits behind (MPEIN) and
 * needs the diagnostic entitlement, because it goes through raw ACCESS_REG.
 *
 * Usage: mlx_port_counters [--watch seconds] [--pcie] [--device mlx5_0]
 */
#include "librdma_shim.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* mlx5 registers address bits big-endian from the start of the payload. */
static uint64_t reg_bits(const uint8_t *buf, uint32_t offset, uint32_t width)
{
    uint64_t value = 0;
    for (uint32_t i = 0; i < width; i++) {
        const uint32_t bit = offset + i;
        value = (value << 1) | ((buf[bit >> 3] >> (7 - (bit & 7))) & 1u);
    }
    return value;
}

static void print_stats(const char *label, const struct rdma_port_stats *s)
{
    printf("%s\n", label);
    printf("  rx  packets=%-14" PRIu64 " bytes=%-16" PRIu64 " errors=%-8" PRIu64 " discards=%" PRIu64 "\n",
           s->rx_pkts, s->rx_bytes, s->rx_errors, s->rx_drop);
    printf("  tx  packets=%-14" PRIu64 " bytes=%-16" PRIu64 " errors=%-8" PRIu64 " discards=%" PRIu64 "\n",
           s->tx_pkts, s->tx_bytes, s->tx_errors, s->tx_drop);
    printf("  pause rx=%" PRIu64 " tx=%" PRIu64 "\n", s->rx_pause, s->tx_pause);
}

static void print_delta(const struct rdma_port_stats *a,
                        const struct rdma_port_stats *b, double seconds)
{
    const uint64_t rx = b->rx_bytes - a->rx_bytes, tx = b->tx_bytes - a->tx_bytes;
    printf("delta over %.1f s\n", seconds);
    printf("  rx  packets=%-14" PRIu64 " bytes=%-16" PRIu64 " %8.2f Gbit/s\n",
           b->rx_pkts - a->rx_pkts, rx, seconds > 0 ? 8.0 * (double)rx / 1e9 / seconds : 0.0);
    printf("  tx  packets=%-14" PRIu64 " bytes=%-16" PRIu64 " %8.2f Gbit/s\n",
           b->tx_pkts - a->tx_pkts, tx, seconds > 0 ? 8.0 * (double)tx / 1e9 / seconds : 0.0);
    printf("  rx errors=%" PRIu64 " discards=%" PRIu64 "  tx errors=%" PRIu64 " discards=%" PRIu64 "\n",
           b->rx_errors - a->rx_errors, b->rx_drop - a->rx_drop,
           b->tx_errors - a->tx_errors, b->tx_drop - a->tx_drop);
    printf("  pause rx=%" PRIu64 " tx=%" PRIu64 "\n",
           b->rx_pause - a->rx_pause, b->tx_pause - a->tx_pause);
}

/* MPCNT group 0 counter set starts at payload byte 8; every field is 32 bit.
 * The stall counters are what a slow host link looks like from the device. */
struct pcie_perf {
    uint32_t outbound_stalled_reads;
    uint32_t outbound_stalled_writes;
    uint32_t outbound_stalled_read_events;
    uint32_t outbound_stalled_write_events;
    uint32_t crc_error_tlp;
    uint32_t tx_overflow_low;
};

static int read_pcie_perf(rdma_device *dev, struct pcie_perf *out)
{
    uint8_t mpcnt[136] = {};
    /* pcie_index at bit 0x08 (8b), grp at bit 0x1a (6b); group 0 is perf. */
    const int rc = rdma_access_reg(dev, 0x9051 /* MPCNT */, 0, 0, mpcnt, sizeof(mpcnt));
    if (rc) return rc;
    out->crc_error_tlp                  = (uint32_t)reg_bits(mpcnt, (8 + 36) * 8, 32);
    out->tx_overflow_low                = (uint32_t)reg_bits(mpcnt, (8 + 44) * 8, 32);
    out->outbound_stalled_reads         = (uint32_t)reg_bits(mpcnt, (8 + 48) * 8, 32);
    out->outbound_stalled_writes        = (uint32_t)reg_bits(mpcnt, (8 + 52) * 8, 32);
    out->outbound_stalled_read_events   = (uint32_t)reg_bits(mpcnt, (8 + 56) * 8, 32);
    out->outbound_stalled_write_events  = (uint32_t)reg_bits(mpcnt, (8 + 60) * 8, 32);
    return 0;
}

static void print_pcie_perf(const char *label, const struct pcie_perf *p)
{
    printf("%s stalled reads=%u writes=%u  events r=%u w=%u  tlp_crc=%u tx_overflow=%u\n",
           label, p->outbound_stalled_reads, p->outbound_stalled_writes,
           p->outbound_stalled_read_events, p->outbound_stalled_write_events,
           p->crc_error_tlp, p->tx_overflow_low);
}

static void print_pcie(rdma_device *dev)
{
    uint8_t mpein[64] = {};
    const int rc = rdma_access_reg(dev, 0x9050 /* MPEIN */, 0, 0, mpein, sizeof(mpein));
    if (rc) {
        printf("pcie: MPEIN unavailable (%d)%s\n", rc,
               rc == -EPERM ? " - needs the diagnostic entitlement" : "");
        return;
    }
    /* MPEIN: link_width_active at bit 0x68 (8b), link_speed_active at 0x70 (16b).
     * The speed field is a mask: bit0 2.5, bit1 5, bit2 8, bit3 16, bit4 32 GT/s. */
    const uint64_t width = reg_bits(mpein, 0x68, 8);
    const uint64_t speed = reg_bits(mpein, 0x70, 16);
    static const char *gen[] = {"2.5 GT/s Gen1", "5 GT/s Gen2", "8 GT/s Gen3",
                                "16 GT/s Gen4", "32 GT/s Gen5"};
    const char *name = "unknown";
    for (int i = 0; i < 5; i++) if (speed & (1u << i)) name = gen[i];
    printf("pcie: link width x%" PRIu64 "  speed %s (mask 0x%" PRIx64 ")\n",
           width, name, speed);

    struct pcie_perf perf = {};
    if (read_pcie_perf(dev, &perf) == 0) print_pcie_perf("pcie:", &perf);
    else printf("pcie: MPCNT unavailable\n");
}

int main(int argc, char **argv)
{
    const char *device = NULL;
    double watch = 0.0;
    int pcie = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--watch") && i + 1 < argc) watch = atof(argv[++i]);
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) device = argv[++i];
        else if (!strcmp(argv[i], "--pcie")) pcie = 1;
        else { fprintf(stderr, "usage: %s [--watch seconds] [--pcie] [--device name]\n", argv[0]); return 2; }
    }

    rdma_device *dev = device ? rdma_open_device_by_name(device) : rdma_open_device();
    if (!dev) { fprintf(stderr, "no MlxRDMA device\n"); return 3; }

    struct rdma_port_stats first = {};
    if (rdma_query_port_stats(dev, &first)) {
        fprintf(stderr, "port stats unavailable: the DEXT may predate them\n");
        rdma_close_device(dev);
        return 4;
    }
    printf("port %u link %s speed %u\n", first.port_num,
           first.link_state ? "up" : "down", first.link_speed);
    if (pcie) print_pcie(dev);

    if (watch <= 0.0) {
        print_stats("counters", &first);
        rdma_close_device(dev);
        return 0;
    }
    struct pcie_perf perf_before = {};
    const int have_perf = pcie && read_pcie_perf(dev, &perf_before) == 0;
    usleep((useconds_t)(watch * 1e6));
    struct rdma_port_stats second = {};
    if (rdma_query_port_stats(dev, &second)) {
        fprintf(stderr, "second sample failed\n");
        rdma_close_device(dev);
        return 4;
    }
    print_delta(&first, &second, watch);
    struct pcie_perf perf_after = {};
    if (have_perf && read_pcie_perf(dev, &perf_after) == 0) {
        const struct pcie_perf d = {
            perf_after.outbound_stalled_reads - perf_before.outbound_stalled_reads,
            perf_after.outbound_stalled_writes - perf_before.outbound_stalled_writes,
            perf_after.outbound_stalled_read_events - perf_before.outbound_stalled_read_events,
            perf_after.outbound_stalled_write_events - perf_before.outbound_stalled_write_events,
            perf_after.crc_error_tlp - perf_before.crc_error_tlp,
            perf_after.tx_overflow_low - perf_before.tx_overflow_low,
        };
        print_pcie_perf("  pcie delta:", &d);
    }
    rdma_close_device(dev);
    return 0;
}
