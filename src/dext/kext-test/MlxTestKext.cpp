/*
 * MlxTestKext.cpp — Standalone Phase-1a test kext for MelonDMA.
 *
 * PURPOSE: validate mlx5 init-segment MMIO reads (fw_rev, cmdif_rev,
 * cmdq params) on the live ConnectX-4 Lx WITHOUT displacing Apple's
 * DriverKit_AppleEthernetMLX5 dext.
 *
 * HOW: matches the PCI device in a SEPARATE match category
 * (com.mlx5.test) so IOKit arbitration allows it to attach alongside
 * the dext. Does NOT call Open() (the dext owns the exclusive session);
 * BAR0 mapping via mapDeviceMemoryWithRegister needs no Open.
 *
 * Gate P1 = "MlxTestKext: fw_rev=X.Y.Z" in the kernel log.
 *
 * ponytail: test-only tool, no error recovery beyond logging —
 * if it fails, read the log and fix; do not build retries here.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSByteOrder.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>

#define super IOService

/* Class declaration (single-file kext: no separate header) */
class MlxTestKext : public IOService
{
    OSDeclareDefaultStructors(MlxTestKext);

public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
};

/* kext module identity — the _kmod_info symbol kmutil requires.
 * start/stop funcs are stubs; IOKit class registration happens via
 * OSDefineMetaClassAndStructors below. */
extern "C" {
static kern_return_t com_mlx5_test_module_start(kmod_info_t *ki, void *data) { return KERN_SUCCESS; }
static kern_return_t com_mlx5_test_module_stop(kmod_info_t *ki, void *data)  { return KERN_SUCCESS; }
KMOD_DECL(com_mlx5_test, "1");
}

OSDefineMetaClassAndStructors(MlxTestKext, IOService);

/* ---- mlx5 init segment (BAR0 + 0x1000, big-endian) ---- */
#define MLX5_INIT_SEG_OFFSET        0x1000
#define MLX5_ISEG_FW_REV_MAJOR      0x000
#define MLX5_ISEG_FW_REV_MINOR      0x004
#define MLX5_ISEG_FW_REV_SUBMINOR   0x008
#define MLX5_ISEG_CMDIF_REV         0x00C
#define MLX5_ISEG_CMDQ_PARAMS       0x010   /* [20:16]=log_size [12:8]=log_stride */

#define VF_LOG(fmt, args...)  IOLog("MlxTestKext: " fmt "\n", ## args)

static inline uint32_t
isegRead32(volatile uint8_t *iseg, uint32_t off)
{
	return OSSwapBigToHostInt32(*(volatile uint32_t *)(iseg + off));
}

bool
MlxTestKext::start(IOService *provider)
{
	IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci) {
		VF_LOG("provider is not IOPCIDevice, abort");
		return false;
	}

	if (!super::start(provider)) return false;

	/* vendor/device sanity */
	UInt16 vendor = pci->configRead16(kIOPCIConfigVendorID);
	UInt16 device = pci->configRead16(kIOPCIConfigDeviceID);
	VF_LOG("matched vendor=0x%04x device=0x%04x", vendor, device);

	/* Map BAR0 — MMIO must be UNCACHED. mapDeviceMemoryWithRegister takes
	 * IODeviceMemory::map(options) options directly; kIOMapCacheModeInhibit
	 * (== kIOMemoryMapCacheModeInhibit) is the one we need. */
	IOMemoryMap *bar0 = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0,
	    kIOMemoryMapCacheModeInhibit);
	if (!bar0) {
		VF_LOG("FATAL: BAR0 map failed (ret=%p)", bar0);
		return false;
	}
	if (bar0->getLength() <= MLX5_INIT_SEG_OFFSET + 0x200) {
		VF_LOG("FATAL: BAR0 too small (%llu bytes)", bar0->getLength());
		bar0->release();
		return false;
	}

	volatile uint8_t *iseg = (volatile uint8_t *)bar0->getVirtualAddress()
	    + MLX5_INIT_SEG_OFFSET;

	/* ---- GATE P1: first MMIO read of the init segment ---- */
	uint32_t fwMajor    = isegRead32(iseg, MLX5_ISEG_FW_REV_MAJOR);
	uint32_t fwMinor    = isegRead32(iseg, MLX5_ISEG_FW_REV_MINOR);
	uint32_t fwSubminor = isegRead32(iseg, MLX5_ISEG_FW_REV_SUBMINOR);
	uint32_t cmdifRev   = isegRead32(iseg, MLX5_ISEG_CMDIF_REV);
	uint32_t cmdqParams = isegRead32(iseg, MLX5_ISEG_CMDQ_PARAMS);

	VF_LOG("GATE P1 PASS: fw_rev=%u.%u.%04x cmdif_rev=%u",
	    fwMajor, fwMinor, fwSubminor, cmdifRev);
	VF_LOG("cmdq: log_size=%u log_stride=%u (BAR0=%llu bytes @ pa 0x%llx)",
	    (cmdqParams >> 16) & 0x1F, (cmdqParams >> 8) & 0x1F,
	    bar0->getLength(), bar0->getPhysicalAddress());

	/* 'initializing' bit — FW boot state while the Apple dext owns the card */
	uint32_t initializing = isegRead32(iseg, 0x1FC);
	VF_LOG("initializing=0x%08x (%s)", initializing,
	    (initializing & 0x80000000) ? "FW STILL BOOTING" : "fw boot done");

	bar0->release();

	/* Publish RESULTS as IORegistry properties (IOLog is unreliable in
	 * unified log; ioreg is the ground truth for this test). */
	setProperty("MlxTestGateP1", kOSBooleanTrue);
	setProperty("fw_rev_major", fwMajor, 32);
	setProperty("fw_rev_minor", fwMinor, 32);
	setProperty("fw_rev_subminor", fwSubminor, 32);
	setProperty("cmdif_rev", cmdifRev, 32);
	setProperty("cmdq_log_size", (cmdqParams >> 16) & 0x1F, 32);
	setProperty("cmdq_log_stride", (cmdqParams >> 8) & 0x1F, 32);
	setProperty("fw_initializing", (initializing & 0x80000000) ? 1 : 0, 32);
	registerService();

	/* stay loaded for inspection; Stop just detaches */
	return true;
}

void
MlxTestKext::stop(IOService *provider)
{
	VF_LOG("stop");
	super::stop(provider);
}
