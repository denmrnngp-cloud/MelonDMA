/* Diagnostic-only MlxRDMA shim API. Do not ship to regular consumers. */
#ifndef LIBRDMA_SHIM_DIAG_H
#define LIBRDMA_SHIM_DIAG_H

#include "librdma_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

int rdma_dbg_exec(rdma_device *dev, uint32_t opcode, const void *in,
                  uint32_t inSize, void *out, uint32_t outCapacity,
                  uint32_t *outSize, uint32_t timeoutMs);

#ifdef __cplusplus
}
#endif
#endif
