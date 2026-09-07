#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <infiniband/verbs.h>
#include "MlxMetalSlotState.hpp"

/* Objective-C++ / ARC. Owns one persistent MR and an independent PD.
 * QPs passed below MUST be created on this pool's protectionDomain.
 * Only this API may submit work referencing a leased slot. No unsignaled WRs.
 * The caller polls its CQ and supplies each local WC with the original lease.
 * Never infer receiver readiness from a remote sender's WRITE completion.
 *
 * Remote-write target contract (front G, task 2+3): the MR is registered with
 * LOCAL_WRITE | REMOTE_WRITE | REMOTE_READ, so a peer may one-sided WRITE
 * straight into a leased slot via `remoteAddress + slot*slotBytes` and
 * `remoteKey`. A WRITE produces no local completion, so the receiver cannot
 * know the bytes landed. The readiness contract is therefore WRITE_WITH_IMM:
 *   1. the receiver pre-posts a RECV (postReceive:) whose completion will
 *      carry the peer's immediate data (sequence/size);
 *   2. the peer issues RDMA_WRITE_WITH_IMM into the slot + remoteKey;
 *   3. the receiver polls its CQ, gets the RECV completion with imm_data
 *      (completeNetwork:), and only after that submits the Metal consumer
 *      command (submitConsumer:) — never before the IMM is observed.
 * The buffer must be hazard-tracked by the application around the IMM wait:
 * Metal must not read the slot before the RECV completion is observed, and
 * the peer must not WRITE a slot the receiver has not yet leased back. */
@interface MlxRegisteredMetalBuffer : NSObject
@property(nonatomic, readonly) id<MTLBuffer> buffer;
@property(nonatomic, readonly) struct ibv_pd *protectionDomain;
@property(nonatomic, readonly) NSUInteger slotBytes;
@property(nonatomic, readonly) uint64_t allocationGeneration;
@property(nonatomic, readonly) uint64_t deviceEpoch;
/* One-sided remote access: peer targets remoteAddress + slot*slotBytes with
 * remoteKey. Zero until the pool is initialized. */
@property(nonatomic, readonly) uint64_t remoteAddress;
@property(nonatomic, readonly) uint32_t localKey;
@property(nonatomic, readonly) uint32_t remoteKey;

- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (instancetype)initWithBuffer:(id<MTLBuffer>)buffer
                       context:(struct ibv_context *)context
                     slotBytes:(NSUInteger)slotBytes
                     slotCount:(NSUInteger)slotCount
                         error:(int *)error;
- (int)acquireTransmit:(BOOL)transmit lease:(MlxMetalLease *)lease;
- (int)cancelLease:(MlxMetalLease)lease; /* acquired but never submitted */
/* These methods attach a completion handler, then COMMIT the command buffer.
 * Encode first, do not enqueue/commit yourself. Callback runs on a Metal queue;
 * never wait synchronously for another Metal callback inside it. */
- (int)submitProducer:(id<MTLCommandBuffer>)commands lease:(MlxMetalLease)lease
           completion:(void (^)(int))completion;
- (int)postSend:(MlxMetalLease)lease qp:(struct ibv_qp *)qp bytes:(uint32_t)bytes;
- (int)postWrite:(MlxMetalLease)lease qp:(struct ibv_qp *)qp bytes:(uint32_t)bytes
         remote:(uint64_t)remote rkey:(uint32_t)rkey;
- (int)postReceive:(MlxMetalLease)lease qp:(struct ibv_qp *)qp;
- (int)completeNetwork:(const struct ibv_wc *)wc lease:(MlxMetalLease)lease;
- (int)submitConsumer:(id<MTLCommandBuffer>)commands lease:(MlxMetalLease)lease
           completion:(void (^)(int))completion;
/* Close requires all slots free AND every QP on protectionDomain destroyed.
 * EBUSY/EIO preserves ownership. Dropping a busy object deliberately retains
 * its backing memory; reconnect creates a new pool/epoch, never reuses it. */
- (int)close;
@end
