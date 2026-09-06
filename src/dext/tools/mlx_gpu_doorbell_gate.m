// mlx_gpu_doorbell_gate.m — Feasibility gate (step 0): can a Metal GPU shader
// write to the UAR (doorbell) VA that the DEXT mapped via IOConnectMapMemory64,
// and does the NIC see it?
//
// RESULT (kernel panic, RELEASE_ARM64_T6020 = M2 Ultra):
//   Phase B panics the whole machine:
//     "physical page is before the start of DRAM: 0x290004 < 0x4000000) @vm_resident.c:3369"
//     Panicked task: mlx_gpu_doorbell_gate
//     backtrace kexts: com.apple.iokit.IOGPUFamily, com.apple.AGXG14X
//   The UAR VA is a PCIe BAR MMIO mapping (not DRAM). Wrapping it in a Metal
//   buffer via newBufferWithBytesNoCopy: makes the Apple GPU driver dereference
//   the MMIO physical page as a resident DRAM page → hard panic.
//   CONCLUSION: GPU cannot write PCIe MMIO on Apple Silicon. GPU-native doorbell
//   is CLOSED; Metal has no cudaHostRegisterIoMemory / mapped-BAR equivalent.
//   Phase B is REMOVED so this tool can never panic the kernel again.
//
// Phases:
//   A) DRAM sanity — shader writes a Metal buffer, CPU reads back (proves the
//      compute pipeline works at all).
//   B) UAR write    — CLOSED (see RESULT above). Do NOT re-add.
//   C) (next step)  — real QP + WQE + doorbell via GPU + poll completion. This
//      is not reachable: the GPU cannot ring the NIC doorbell (Phase B closed).
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "librdma_shim.h"
#include <stdio.h>
#include <string.h>

static NSString * kSrc = @""
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void poke(device uint64_t *dst [[buffer(0)]],\n"
    "                 constant uint32_t &off8 [[buffer(1)]],\n"
    "                 constant uint64_t &val [[buffer(2)]],\n"
    "                 uint id [[thread_position_in_grid]]) {\n"
    "    if (id == 0) dst[off8] = val;\n"
    "}\n";

int main(void) {
    int rc = 1;

    // --- 1. Map the UAR via the shim (needs a PD so EnableFastPath passes). ---
    rdma_device * dev = rdma_open_device();
    if (!dev) { fprintf(stderr, "FAIL: rdma_open_device\n"); return 1; }
    rdma_pd * pd = rdma_alloc_pd(dev);
    if (!pd) { fprintf(stderr, "FAIL: rdma_alloc_pd\n"); return 1; }
    struct rdma_fast_path path = {};
    if (rdma_enable_fast_path(dev, &path) != 0) {
        fprintf(stderr, "FAIL: rdma_enable_fast_path\n"); return 1;
    }
    if (rdma_map_fast_path(dev, &path) != 0) {
        fprintf(stderr, "FAIL: rdma_map_fast_path\n"); return 1;
    }
    void * uar = path.uar;
    printf("UAR VA = %p (page %u bytes)\n", uar, path.uar_page_size);
    if (!uar || ((uintptr_t)uar & 0xfff)) {
        fprintf(stderr, "FAIL: UAR VA not page-aligned\n"); return 1;
    }

    // --- 2. Metal device + pipeline ---
    id<MTLDevice> mdev = MTLCreateSystemDefaultDevice();
    if (!mdev) { fprintf(stderr, "FAIL: no Metal device\n"); return 1; }
    id<MTLCommandQueue> queue = [mdev newCommandQueue];
    id<MTLLibrary> lib = [mdev newLibraryWithSource:kSrc options:nil error:nil];
    if (!lib) { fprintf(stderr, "FAIL: shader compile\n"); return 1; }
    id<MTLFunction> fn = [lib newFunctionWithName:@"poke"];
    id<MTLComputePipelineState> pipe = [mdev newComputePipelineStateWithFunction:fn error:nil];
    if (!pipe) { fprintf(stderr, "FAIL: pipeline\n"); return 1; }

    // --- Phase A: DRAM sanity. ---
    uint64_t dram[1] = {0};
    id<MTLBuffer> dramBuf = [mdev newBufferWithBytesNoCopy:dram length:8
        options:MTLResourceStorageModeShared deallocator:nil];
    {
        uint32_t off8 = 0; uint64_t val = 0x123456789abcdef0ull;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pipe];
        [enc setBuffer:dramBuf offset:0 atIndex:0];
        [enc setBytes:&off8 length:4 atIndex:1];
        [enc setBytes:&val length:8 atIndex:2];
        [enc dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.error) { fprintf(stderr, "FAIL: DRAM shader error %s\n",
                                cb.error.description.UTF8String); return 1; }
        printf("Phase A (DRAM): shader wrote 0x%016llx -> CPU reads 0x%016llx %s\n",
               (unsigned long long)val, (unsigned long long)dram[0],
               dram[0] == val ? "OK" : "MISMATCH");
        if (dram[0] != val) { fprintf(stderr, "FAIL: DRAM round-trip\n"); return 1; }
    }

    // --- Phase B: UAR doorbell via GPU — CLOSED. ---------------------------
    // Wrapping the UAR VA (PCIe BAR MMIO) in a Metal buffer via
    // newBufferWithBytesNoCopy: hard-panics the kernel (IOGPUFamily/AGXG14X
    // dereferences the MMIO phys page 0x290004 < DRAM start 0x4000000).
    // Do not re-add. See header RESULT + docs/apple-silicon-metal-dma.md §7.1.
    printf("Phase B (UAR doorbell via GPU): CLOSED — Apple GPU cannot write "
           "PCIe MMIO; probing via bytesNoCopy panics the kernel (see source).\n");

    // --- Phase C: real doorbell + completion — NOT reachable. ---
    // GPU cannot ring the NIC doorbell (Phase B closed), so there is no GPU
    // data path. CPU-proxy remains; ceiling is PCIe Gen3 x4 pacing, not CPU.
    printf("Phase C (GPU doorbell + CQ completion): not reachable — Phase B closed.\n");
    printf("FEASIBILITY: GPU-native doorbell CLOSED on Apple Silicon (no Metal MMIO).\n");
    rc = 0;

    if (pd) rdma_dealloc_pd(pd);
    if (dev) rdma_close_device(dev);
    return rc;
}
