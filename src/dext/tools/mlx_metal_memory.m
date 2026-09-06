/*
 * Metal-backed memory provider for mlx_phase2_gate.c.
 *
 * The returned pointer is MTLBuffer.contents() from a shared, untracked
 * buffer. Registering that pointer through ibv_reg_mr exercises the real
 * Apple-Silicon zero-copy path: Metal and ConnectX access the same UMA pages,
 * while command-buffer completion and RDMA CQ/control completion provide the
 * ordering boundary in each direction.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdint.h>
#include <stdio.h>

static id<MTLDevice> sDevice;
static id<MTLBuffer> sData;
static id<MTLComputePipelineState> sFillPipeline;
static id<MTLComputePipelineState> sVerifyPipeline;

static int metal_prepare_pipelines(void)
{
    if (sFillPipeline && sVerifyPipeline) return 0;
    static NSString *source =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "kernel void fill_bytes(device uchar *data [[buffer(0)]],\n"
         "                       constant uchar &value [[buffer(1)]],\n"
         "                       constant uint &length [[buffer(2)]],\n"
         "                       uint gid [[thread_position_in_grid]]) {\n"
         "    if (gid < length) data[gid] = value;\n"
         "}\n"
         "kernel void verify_bytes(device const uchar *data [[buffer(0)]],\n"
         "                         constant uchar &expected [[buffer(1)]],\n"
         "                         constant uint &length [[buffer(2)]],\n"
         "                         device atomic_uint *errors [[buffer(3)]],\n"
         "                         uint gid [[thread_position_in_grid]]) {\n"
         "    if (gid < length && data[gid] != expected)\n"
         "        atomic_fetch_add_explicit(errors, 1u, memory_order_relaxed);\n"
         "}\n";
    NSError *error = nil;
    id<MTLLibrary> library = [sDevice newLibraryWithSource:source
                                                   options:nil
                                                     error:&error];
    if (!library) {
        fprintf(stderr, "Metal shader compile failed: %s\n",
                error.localizedDescription.UTF8String ?: "unknown error");
        return -1;
    }
    sFillPipeline = [sDevice newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"fill_bytes"] error:&error];
    if (!sFillPipeline) {
        fprintf(stderr, "Metal fill pipeline failed: %s\n",
                error.localizedDescription.UTF8String ?: "unknown error");
        return -1;
    }
    sVerifyPipeline = [sDevice newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"verify_bytes"] error:&error];
    if (!sVerifyPipeline) {
        fprintf(stderr, "Metal verify pipeline failed: %s\n",
                error.localizedDescription.UTF8String ?: "unknown error");
        return -1;
    }
    return 0;
}

static int metal_range(const void *address, size_t length, NSUInteger *offset)
{
    if (!sData || !address || !length || length > UINT32_MAX) return -1;
    const uintptr_t base = (uintptr_t)sData.contents;
    const uintptr_t start = (uintptr_t)address;
    if (start < base || start - base > sData.length ||
        length > sData.length - (start - base)) return -1;
    *offset = (NSUInteger)(start - base);
    return 0;
}

static int wait_for_command(id<MTLCommandBuffer> command, const char *operation)
{
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "Metal %s failed: %s\n", operation,
                command.error.localizedDescription.UTF8String ?: "unknown error");
        return -1;
    }
    return 0;
}

int mlx_gate_memory_allocate(size_t length, void **address)
{
    @autoreleasepool {
        if (!address || !length || sData) return -1;
        sDevice = MTLCreateSystemDefaultDevice();
        if (!sDevice) {
            fprintf(stderr, "Metal DMA gate: no Metal device\n");
            return -1;
        }
        if ([sDevice respondsToSelector:@selector(hasUnifiedMemory)] &&
            !sDevice.hasUnifiedMemory) {
            fprintf(stderr, "Metal DMA gate requires Apple unified memory\n");
            return -1;
        }

        const MTLResourceOptions options = MTLResourceStorageModeShared |
            MTLResourceHazardTrackingModeUntracked;
        sData = [sDevice newBufferWithLength:length options:options];
        if (!sData || !sData.contents || ((uintptr_t)sData.contents & 4095u)) {
            fprintf(stderr, "Metal DMA gate: shared contents unavailable or not 4 KiB aligned\n");
            sData = nil;
            return -1;
        }

        /* Private storage must never be offered to ibv_reg_mr. Confirm the
         * platform keeps it CPU-inaccessible without attempting any MMIO/BAR
         * wrapping (which is explicitly unsafe on Apple Silicon). */
        id<MTLBuffer> privateBuffer = [sDevice newBufferWithLength:4096
            options:MTLResourceStorageModePrivate];
        BOOL privateInaccessible = NO;
        @try {
            privateInaccessible = privateBuffer.contents == NULL;
        } @catch (NSException *exception) {
            (void)exception;
            privateInaccessible = YES;
        }
        /* Some Apple GPU driver builds return an opaque non-NULL value here
         * even though private storage has no supported CPU/DMA access
         * contract. Do not dereference or register it; only record the
         * observation. The accepted MR below is always the shared buffer. */
        if (metal_prepare_pipelines()) {
            sData = nil;
            return -1;
        }
        *address = sData.contents;
        printf("METAL_UMA_MR: device=%s shared=1 untracked=1 private_contents=%s bytes=%zu va=%p\n",
               sDevice.name.UTF8String,
               privateInaccessible ? "inaccessible" : "opaque-nonnull",
               length, *address);
        return 0;
    }
}

int mlx_gate_memory_gpu_fill(void *address, size_t length, uint8_t value)
{
    @autoreleasepool {
        NSUInteger offset = 0;
        if (metal_range(address, length, &offset) || metal_prepare_pipelines())
            return -1;
        id<MTLCommandQueue> queue = [sDevice newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (!command || !encoder) return -1;
        uint32_t count = (uint32_t)length;
        [encoder setComputePipelineState:sFillPipeline];
        [encoder setBuffer:sData offset:offset atIndex:0];
        [encoder setBytes:&value length:sizeof(value) atIndex:1];
        [encoder setBytes:&count length:sizeof(count) atIndex:2];
        NSUInteger width = MIN(sFillPipeline.maxTotalThreadsPerThreadgroup, 256u);
        [encoder dispatchThreads:MTLSizeMake(length, 1, 1)
             threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        [encoder endEncoding];
        if (wait_for_command(command, "GPU->NIC producer fill")) return -1;
        printf("METAL_GPU_PRODUCER PASS: bytes=%zu value=0x%02x\n", length, value);
        return 0;
    }
}

int mlx_gate_memory_gpu_verify(const void *address, size_t length,
                               uint8_t expected)
{
    @autoreleasepool {
        NSUInteger offset = 0;
        if (metal_range(address, length, &offset) || metal_prepare_pipelines())
            return -1;
        id<MTLBuffer> errors = [sDevice newBufferWithLength:sizeof(uint32_t)
            options:MTLResourceStorageModeShared |
                    MTLResourceHazardTrackingModeUntracked];
        if (!errors || !errors.contents) return -1;
        *(uint32_t *)errors.contents = 0;
        id<MTLCommandQueue> queue = [sDevice newCommandQueue];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (!command || !encoder) return -1;
        uint32_t count = (uint32_t)length;
        [encoder setComputePipelineState:sVerifyPipeline];
        [encoder setBuffer:sData offset:offset atIndex:0];
        [encoder setBytes:&expected length:sizeof(expected) atIndex:1];
        [encoder setBytes:&count length:sizeof(count) atIndex:2];
        [encoder setBuffer:errors offset:0 atIndex:3];
        NSUInteger width = MIN(sVerifyPipeline.maxTotalThreadsPerThreadgroup, 256u);
        [encoder dispatchThreads:MTLSizeMake(length, 1, 1)
             threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        [encoder endEncoding];
        if (wait_for_command(command, "NIC->GPU consumer verify")) return -1;
        const uint32_t mismatches = *(const uint32_t *)errors.contents;
        if (mismatches) {
            fprintf(stderr, "METAL_GPU_CONSUMER FAIL: mismatches=%u/%zu expected=0x%02x\n",
                    mismatches, length, expected);
            return -1;
        }
        printf("METAL_GPU_CONSUMER PASS: bytes=%zu expected=0x%02x mismatches=0\n",
               length, expected);
        return 0;
    }
}

void mlx_gate_memory_release(void *address)
{
    (void)address;
    sVerifyPipeline = nil;
    sFillPipeline = nil;
    sData = nil;
    sDevice = nil;
}
