/* mlx_gate_report — one command, one output: every counter a transfer claim
 * needs, side by side (front F, task 3).
 *
 * Combines:
 *   - port counters (PPCNT): the card's own wire view, incl. pause/discard;
 *   - PCIe perf counters (MPCNT): stalled reads/writes, TLP CRC, overflow —
 *     the slow-host-link view, needs the diagnostic entitlement, skipped
 *     gracefully without it;
 *   - the negotiated PCIe line (from QueryLimits) and eta = achieved payload
 *     rate as a fraction of that line, so a number means the same on any card;
 *   - per-operation counters (perf + stats): doorbells, CQEs, MR registrations,
 *     firmware commands, and the RNR/retry counts that surface congestion;
 *   - DCQCN reaction-point params.
 *
 * Snapshot mode prints raw counters. --watch N prints deltas over N seconds
 * and computes Gbit/s and eta; that is the mode a measurement is made in.
 *
 * Usage: mlx_gate_report [--watch seconds]
 */
#include "librdma_shim.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Big-endian bit field read straight out of the register payload. */
static uint64_t reg_bits(const uint8_t *buf, uint32_t offset, uint32_t width)
{
    uint64_t value = 0;
    for (uint32_t i = 0; i < width; i++) {
        const uint32_t bit = offset + i;
        value = (value << 1) | ((buf[bit >> 3] >> (7 - (bit & 7))) & 1u);
    }
    return value;
}

struct pcie_perf {
    uint32_t stalled_reads, stalled_writes;
    uint32_t stalled_read_events, stalled_write_events;
    uint32_t crc_error_tlp, tx_overflow_low;
};

static int read_pcie_perf(rdma_device *dev, struct pcie_perf *out)
{
    uint8_t mpcnt[136] = {};
    const int rc = rdma_access_reg(dev, 0x9051 /* MPCNT */, 0, 0,
                                   mpcnt, sizeof(mpcnt));
    if (rc) return rc;
    out->crc_error_tlp          = (uint32_t)reg_bits(mpcnt, (8 + 36) * 8, 32);
    out->tx_overflow_low        = (uint32_t)reg_bits(mpcnt, (8 + 44) * 8, 32);
    out->stalled_reads          = (uint32_t)reg_bits(mpcnt, (8 + 48) * 8, 32);
    out->stalled_writes         = (uint32_t)reg_bits(mpcnt, (8 + 52) * 8, 32);
    out->stalled_read_events    = (uint32_t)reg_bits(mpcnt, (8 + 56) * 8, 32);
    out->stalled_write_events   = (uint32_t)reg_bits(mpcnt, (8 + 60) * 8, 32);
    return 0;
}

static void print_pcie_delta(const struct pcie_perf *a, const struct pcie_perf *b)
{
    printf("pcie   stalled rd=%u wr=%u ev_rd=%u ev_wr=%u tlp_crc=%u tx_ovf=%u\n",
           b->stalled_reads - a->stalled_reads,
           b->stalled_writes - a->stalled_writes,
           b->stalled_read_events - a->stalled_read_events,
           b->stalled_write_events - a->stalled_write_events,
           b->crc_error_tlp - a->crc_error_tlp,
           b->tx_overflow_low - a->tx_overflow_low);
}

int main(int argc, char **argv)
{
    double watch = 0.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--watch") && i + 1 < argc) watch = atof(argv[++i]);
        else { fprintf(stderr, "usage: %s [--watch seconds]\n", argv[0]); return 2; }
    }

    rdma_device *dev = rdma_open_device();
    if (!dev) { fprintf(stderr, "no MlxRDMA device\n"); return 3; }

    struct rdma_limits lim = {};
    double line = 0.0;
    if (rdma_query_limits(dev, &lim) == 0)
        line = rdma_pcie_line_gbps(lim.pcie_link_speed, lim.pcie_link_width);

    struct rdma_port_stats p0 = {}, p1 = {};
    if (rdma_query_port_stats(dev, &p0)) {
        fprintf(stderr, "port stats unavailable\n");
        rdma_close_device(dev);
        return 4;
    }

    struct pcie_perf pc0 = {}, pc1 = {};
    const int have_pcie = read_pcie_perf(dev, &pc0) == 0;

    if (watch > 0.0) usleep((useconds_t)(watch * 1e6));

    struct rdma_port_stats *p = &p0;
    if (watch > 0.0) {
        if (rdma_query_port_stats(dev, &p1)) { fprintf(stderr, "second sample failed\n"); return 4; }
        p = &p1;
        if (have_pcie) (void)read_pcie_perf(dev, &pc1);
    }

    /* link_speed carries max_tx_speed raw, in units of 100 Mbps. */
    printf("link   port %u %s speed %.1f Gbit/s\n", p->port_num,
           p->link_state ? "up" : "down", p->link_speed / 10.0);
    if (line > 0.0)
        printf("line   PCIe gen%u x%u, %.1f Gbit/s per direction\n",
               lim.pcie_link_speed, lim.pcie_link_width, line);

    if (watch > 0.0) {
        const uint64_t rx = p1.rx_bytes - p0.rx_bytes;
        const uint64_t tx = p1.tx_bytes - p0.tx_bytes;
        printf("port   rx pkts=%-12" PRIu64 " bytes=%-16" PRIu64 " %8.2f Gbit/s\n",
               p1.rx_pkts - p0.rx_pkts, rx, 8.0 * (double)rx / 1e9 / watch);
        printf("       tx pkts=%-12" PRIu64 " bytes=%-16" PRIu64 " %8.2f Gbit/s\n",
               p1.tx_pkts - p0.tx_pkts, tx, 8.0 * (double)tx / 1e9 / watch);
        printf("       rx errs=%" PRIu64 " discards=%" PRIu64 "  tx errs=%" PRIu64
               " discards=%" PRIu64 "\n",
               p1.rx_errors - p0.rx_errors, p1.rx_drop - p0.rx_drop,
               p1.tx_errors - p0.tx_errors, p1.tx_drop - p0.tx_drop);
        printf("       pause rx=%" PRIu64 " tx=%" PRIu64 "\n",
               p1.rx_pause - p0.rx_pause, p1.tx_pause - p0.tx_pause);
        if (line > 0.0) {
            const double rxG = 8.0 * (double)rx / 1e9 / watch;
            const double txG = 8.0 * (double)tx / 1e9 / watch;
            printf("eta    rx %.2f%%  tx %.2f%% of negotiated line\n",
                   100.0 * rxG / line, 100.0 * txG / line);
        }
        if (have_pcie) print_pcie_delta(&pc0, &pc1);
    } else {
        printf("port   rx pkts=%" PRIu64 " bytes=%" PRIu64 " errs=%" PRIu64
               " discards=%" PRIu64 "\n",
               p->rx_pkts, p->rx_bytes, p->rx_errors, p->rx_drop);
        printf("       tx pkts=%" PRIu64 " bytes=%" PRIu64 " errs=%" PRIu64
               " discards=%" PRIu64 "\n",
               p->tx_pkts, p->tx_bytes, p->tx_errors, p->tx_drop);
        printf("       pause rx=%" PRIu64 " tx=%" PRIu64 "\n",
               p->rx_pause, p->tx_pause);
        if (have_pcie)
            printf("pcie   stalled rd=%u wr=%u ev_rd=%u ev_wr=%u tlp_crc=%u tx_ovf=%u\n",
                   pc0.stalled_reads, pc0.stalled_writes,
                   pc0.stalled_read_events, pc0.stalled_write_events,
                   pc0.crc_error_tlp, pc0.tx_overflow_low);
    }

    struct rdma_perf perf = {};
    if (rdma_query_perf(dev, &perf) == 0) {
        printf("perf   doorbells=%" PRIu64 " cqe=%" PRIu64 " cqe_errs=%" PRIu64
               " mr_reg=%" PRIu64 " mr_dereg=%" PRIu64 "\n",
               perf.doorbells, perf.cqe_consumed, perf.cqe_errors,
               perf.mr_registers, perf.mr_deregisters);
        printf("       fw_cmds=%" PRIu64 " fw_sleeps=%" PRIu64 " copied=%" PRIu64
               " cq_events=%" PRIu64 "\n",
               perf.fw_commands, perf.fw_command_sleeps, perf.copied_bytes,
               perf.cq_events);
    }

    struct rdma_stats stats = {};
    if (rdma_query_stats(dev, &stats) == 0) {
        printf("ops    post s=%" PRIu64 " w=%" PRIu64 " r=%" PRIu64
               "  comp s=%" PRIu64 " w=%" PRIu64 " r=%" PRIu64 "\n",
               stats.posted_send, stats.posted_write, stats.posted_recv,
               stats.completed_send, stats.completed_write, stats.completed_recv);
        printf("       cqe_err=%" PRIu64 " retry_exc=%" PRIu64 " rnr_retry=%" PRIu64
               " cq_lost=%" PRIu64 "  sq=%u rq=%u\n",
               stats.cqe_error, stats.cqe_retry_exc, stats.cqe_rnr_retry,
               stats.cq_lost, stats.sq_occupancy, stats.rq_occupancy);
    }

    struct rdma_cong_params cong = {};
    if (rdma_query_cong(dev, &cong) == 0) {
        printf("dcqcn  min_dec=%u ai=%u time_reset=%u threshold=%u hai=%u gd=%u\n",
               cong.rpg_min_dec_fac, cong.rpg_ai_rate, cong.rpg_time_reset,
               cong.rpg_threshold, cong.rpg_hai, cong.rpg_gd);
    }

    if (!have_pcie && watch <= 0.0)
        printf("pcie   unavailable (needs the diagnostic entitlement)\n");

    rdma_close_device(dev);
    return 0;
}
