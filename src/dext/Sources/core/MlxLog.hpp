/*
 * MlxLog.hpp — build-level logging split.
 *
 * Release builds leave MLX_DEBUG at 0: every MLX_DBGLOG()/MLX_DBG() call
 * compiles out entirely (no IOLog, no argument evaluation). Development
 * builds pass -DMLX_DEBUG=1 (or `make MLX_DEBUG=1`) to keep verbose and
 * diagnostic logs. Error/warning logs always stay on — a release DEXT must
 * still report fatal events, error CQEs, failed destroys and FLR state.
 *
 * Hex dumps of hardware structures (WQE/CQE/QPC/PAS/mailbox) are removed
 * from the tree entirely; re-add locally under MLX_DEBUG if a specific
 * bring-up needs them again.
 */
#ifndef MLX_LOG_HPP
#define MLX_LOG_HPP

#include <DriverKit/IOLib.h>

#ifndef MLX_DEBUG
#define MLX_DEBUG 0
#endif

#if MLX_DEBUG
#define MLX_DBGLOG(fmt, ...) IOLog(fmt "\n", ##__VA_ARGS__)
#else
#define MLX_DBGLOG(fmt, ...) ((void)0)
#endif

#endif /* MLX_LOG_HPP */
