/* Rebind the completion EQ to a chosen MSI-X index, for one experiment:
 * does this dext ever receive an interrupt, and on which vector?
 *
 * Run it with no CQs alive, then drive traffic (run_cq_event_gate.sh), then
 * read mlx_perf_test: async_irq moves if the events landed on vector 0,
 * completion_irq if they landed on vector 1, neither if MSI-X is not
 * delivered to this dext at all.
 *
 * Usage: mlx_irq_probe <0|1>
 */
#include "librdma_shim.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s <0|1>\n", argv[0]); return 2; }
    uint32_t intr = (uint32_t)strtoul(argv[1], NULL, 10);
    rdma_device *dev = rdma_open_device();
    if (!dev) { fprintf(stderr, "rdma_open_device failed\n"); return 1; }
    uint32_t eqn = 0;
    int rc = rdma_probe_completion_vector(dev, intr, &eqn);
    if (rc == 0) printf("IRQ_PROBE: completion EQ %u now on intr=%u\n", eqn, intr);
    else if (rc == -EBUSY) printf("IRQ_PROBE: refused, a CQ is still live\n");
    else if (rc == -ENOTSUP) printf("IRQ_PROBE: provider predates the probe\n");
    else printf("IRQ_PROBE: failed rc=%d\n", rc);
    struct rdma_interrupt_attr irq = {};
    if (rdma_query_interrupts(dev, &irq) == 0)
        printf("IRQ_PROBE: async_irq=%llu completion_irq=%llu completion_eqn=%u\n",
               (unsigned long long)irq.async_interrupts,
               (unsigned long long)irq.completion_interrupts,
               irq.completion_eqn);
    rdma_close_device(dev);
    return rc == 0 ? 0 : 1;
}
