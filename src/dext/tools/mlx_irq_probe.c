/* Interrupt-path diagnosis for the MSI-X delivery question.
 *
 * There are TWO index spaces and they are easy to confuse:
 *
 *   firmware intr  the vector CREATE_EQ tells the card to raise. Values 0
 *                  (async) and 1 (completion). `--rebind` moves it.
 *   host index     the IOInterruptDispatchSource index the DEXT bound its
 *                  handler to. Firmware vector V arrives on host index
 *                  msix_base + V. Moving the firmware intr can never detect a
 *                  mismatch here; the index map below is what shows it.
 *
 * Default run prints the map and the MSI-X capability, changes nothing.
 *
 * Usage:
 *   mlx_irq_probe                 map + MSI-X state (read-only)
 *   mlx_irq_probe --rebind <0|1>  move the completion EQ to a firmware vector
 *                                 (refused while any CQ is live), then print
 */
#include "librdma_shim.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kind_name(uint8_t kind)
{
    switch (kind) {
    case RDMA_IRQ_KIND_ABSENT: return "-";
    case RDMA_IRQ_KIND_LEVEL:  return "level/INTx";
    case RDMA_IRQ_KIND_EDGE:   return "edge";
    case RDMA_IRQ_KIND_MSI:    return "MSI";
    case RDMA_IRQ_KIND_MSIX:   return "MSI-X";
    default:                   return "other";
    }
}

static void print_index_map(const struct rdma_interrupt_attr *irq)
{
    printf("IRQ_MAP: probed %u indices after allocation, %u before; "
           "probe status 0x%x\n",
           irq->index_count, irq->index_count_pre, irq->index_probe_status);
    printf("  idx  before      after       raw type      bind\n");
    for (unsigned i = 0; i < RDMA_IRQ_INDEX_MAP; i++) {
        if (irq->index_kind[i] == RDMA_IRQ_KIND_ABSENT &&
            irq->index_kind_pre[i] == RDMA_IRQ_KIND_ABSENT)
            continue;
        /* bind is the kern_return_t of a trial dispatch-source Create on an
         * index the driver does not use: 0 means a vector really is available
         * there, not merely allocated and programmed. */
        char bind[24];
        if (irq->index_bind[i] == RDMA_IRQ_BIND_NOT_TRIED)
            snprintf(bind, sizeof(bind), "%-12s", "-");
        else if (irq->index_bind[i] == 0)
            snprintf(bind, sizeof(bind), "%-12s", "BINDS");
        else
            snprintf(bind, sizeof(bind), "0x%-10x", irq->index_bind[i]);
        printf("  %3u  %-10s  %-10s  0x%-9llx  %s%s\n", i,
               kind_name(irq->index_kind_pre[i]),
               kind_name(irq->index_kind[i]),
               (unsigned long long)irq->index_type_raw[i], bind,
               i == irq->async_index ? "  <- async source"
               : i == irq->completion_index ? "  <- completion source" : "");
    }
    if (irq->msix_index_base == RDMA_IRQ_INDEX_NONE)
        printf("IRQ_MAP: no messaged pair found — the DEXT kept the historical "
               "indices %u/%u. On an MSI-X-only device behind a tunnel, index 0 "
               "is legacy INTx and cannot fire.\n",
               irq->async_index, irq->completion_index);
    else
        if (irq->completion_eq_count > 1) {
        printf("COMP_EQ: %u queues, interrupts each:", irq->completion_eq_count);
        for (unsigned i = 0; i < irq->completion_eq_count &&
                             i < RDMA_IRQ_COMP_EQ_MAX; i++)
            printf(" [%u]=%llu", i,
                   (unsigned long long)irq->completion_irq_by_eq[i]);
        printf("\n");
    } else {
        printf("COMP_EQ: 1 queue (no extra completion vectors came up)\n");
    }
    printf("IRQ_MAP: firmware vector V arrives on host index %u + V; "
               "async bound to %u, completion to %u\n",
               irq->msix_index_base, irq->async_index, irq->completion_index);
}

static void print_msix_state(rdma_device *dev)
{
    struct rdma_msix_state st = {};
    int rc = rdma_query_msix_state(dev, &st);
    if (rc == -EPERM) {
        printf("MSIX: needs the privileged diagnostics entitlement\n");
        return;
    }
    if (rc == -ENOTSUP) {
        printf("MSIX: no MSI-X capability, or the provider predates this call\n");
        return;
    }
    if (rc) { printf("MSIX: query failed rc=%d\n", rc); return; }

    unsigned enabled = (st.message_control >> 15) & 1u;
    unsigned masked  = (st.message_control >> 14) & 1u;
    printf("MSIX: cap@0x%02x control=0x%04x enable=%u function_mask=%u "
           "table_size=%u command=0x%04x\n",
           st.cap_offset, st.message_control, enabled, masked,
           st.table_size, st.command_reg);
    printf("MSIX: table BIR %u +0x%x, PBA BIR %u +0x%x, read through memory "
           "index %u\n", st.table_bir, st.table_offset,
           st.pba_bir, st.pba_offset, st.bar_index_used);
    printf("MSIX: PRE-CONFIGURE entry0 addrLo=0x%08x data=0x%08x  %s\n",
           st.pre_configure_entry0_addr_lo, st.pre_configure_entry0_data,
           st.pre_configure_entry0_addr_lo
               ? "previous owner programmed it" : "empty before our ConfigureInterrupts");
    if (st.status) {
        printf("MSIX: table/PBA not read (status 0x%x) — they live in a BAR "
               "this driver does not map\n", st.status);
        return;
    }
    for (unsigned i = 0; i < st.entries_read; i++) {
        const struct rdma_msix_entry *e = &st.entry[i];
        int programmed = (e->addr_lo | e->addr_hi | e->data) != 0;
        printf("  vector %u: addr=0x%08x%08x data=0x%08x ctrl=0x%08x  %s%s\n",
               i, e->addr_hi, e->addr_lo, e->data, e->vector_control,
               programmed ? "programmed" : "NOT PROGRAMMED",
               (e->vector_control & 1u) ? ", masked" : "");
    }
    for (unsigned i = 0; i < st.pba_words; i++)
        printf("  pending[%u..%u] = 0x%08x\n", i * 32, i * 32 + 31, st.pba[i]);

    if (!enabled)
        printf("MSIX: VERDICT enable bit is clear — the kernel never turned "
               "MSI-X on, so no vector can be raised.\n");
    else if (st.entries_read && (st.entry[0].addr_lo | st.entry[0].addr_hi |
                                 st.entry[0].data) == 0)
        printf("MSIX: VERDICT vector 0 is unprogrammed — the kernel wired up "
               "entries other than the ones firmware raises.\n");
    else
        printf("MSIX: VERDICT table looks live. Take this snapshot again after "
               "a traffic burst: a pending bit that latches with no counter "
               "movement means the event was lost past the controller.\n");
}

int main(int argc, char **argv)
{
    int rebind = -1;
    if (argc == 3 && !strcmp(argv[1], "--rebind"))
        rebind = (int)strtoul(argv[2], NULL, 10);
    else if (argc == 2 && argv[1][0] >= '0' && argv[1][0] <= '9')
        rebind = (int)strtoul(argv[1], NULL, 10);   /* historical form */
    else if (argc != 1) {
        fprintf(stderr, "usage: %s [--rebind <vector>]   (0..8 here; see the bind column)\n", argv[0]);
        return 2;
    }

    rdma_device *dev = rdma_open_device();
    if (!dev) { fprintf(stderr, "rdma_open_device failed\n"); return 1; }

    int rc = 0;
    if (rebind >= 0) {
        uint32_t eqn = 0;
        rc = rdma_probe_completion_vector(dev, (uint32_t)rebind, &eqn);
        if (rc == 0)
            printf("IRQ_PROBE: completion EQ %u now on firmware vector %d\n",
                   eqn, rebind);
        else if (rc == -EBUSY)
            printf("IRQ_PROBE: refused, a CQ is still live\n");
        else if (rc == -ENOTSUP)
            printf("IRQ_PROBE: provider predates the probe\n");
        else
            printf("IRQ_PROBE: failed rc=%d\n", rc);
    }

    struct rdma_interrupt_attr irq = {};
    if (rdma_query_interrupts(dev, &irq) == 0) {
        printf("IRQ: vectors=%u async_irq=%llu completion_irq=%llu "
               "completion_eqn=%u timer_ticks=%llu timer_period=%ums "
               "setup_stage=%u setup_status=0x%x\n",
               irq.vectors,
               (unsigned long long)irq.async_interrupts,
               (unsigned long long)irq.completion_interrupts,
               irq.completion_eqn,
               (unsigned long long)irq.eq_timer_ticks,
               irq.eq_timer_period_ms,
               irq.setup_stage, irq.setup_status);
        print_index_map(&irq);
    } else {
        printf("IRQ: query failed\n");
    }
    print_msix_state(dev);

    /* Read-only diagnostics only. MSI-X table/PBA writes belong exclusively
     * to mlx_msix_hunt; this probe must not contaminate the next live test. */
    printf("PBA: read-only snapshot is included above; no writes performed\n");

    /* Firmware's own view of the two event queues. An EQ that reads ARMED
     * while entries are being written into it never fired its vector. */
    for (int k = 0; k < 2; k++) {
        uint32_t eqn = k ? irq.completion_eqn : irq.async_eqn;
        if (!eqn) continue;
        struct rdma_eq_state st = {};
        int erc = rdma_query_eq_state(dev, eqn, &st);
        if (erc) {
            printf("EQ[%s=%u]: query failed rc=%d\n",
                   k ? "completion" : "async", eqn, erc);
            continue;
        }
        if (st.state == 0xffffffffu) {
            printf("EQ[%s=%u]: QUERY_EQ refused, kr=0x%x fw_status=%u "
                   "syndrome=0x%08x\n", k ? "completion" : "async", eqn,
                   st.status, st.fw_status, st.syndrome);
            continue;
        }
        /* log_eq_size is a cross-check on the parse: the driver creates these
         * queues with 256 entries, so anything but 8 means the fields below
         * are being read from the wrong place. */
        printf("EQ[%s=%u]: state=0x%x%s intr=%u uar_page=%u log_size=%u "
               "ci=%u pi=%u%s\n",
               k ? "completion" : "async", eqn, st.state,
               st.state == 0x9 ? " ARMED" : st.state == 0xa ? " FIRED" :
               st.state == 0xb ? " ALWAYS_ARMED" : "",
               st.intr, st.uar_page, st.log_eq_size,
               st.consumer_index, st.producer_index,
               st.log_eq_size == 8 ? "" : "   <- log_size unexpected, parse suspect");
    }

    rdma_close_device(dev);
    return rc == 0 ? 0 : 1;
}
