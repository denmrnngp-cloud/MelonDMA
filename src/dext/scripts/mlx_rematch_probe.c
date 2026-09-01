/*
 * mlx_rematch_probe.c — force rematch of the ConnectX card.
 *
 * When the card is an "orphan" (ethernet@0 without an owner: Apple killed, our
 * dext did not come up), the kernel does NOT restart matching on its own. The
 * only working trigger is IOServiceRequestProbe() on the PCI node (see
 * notes/27, mlxdetach.c). It re-runs probe/matching on the IOPCIDevice
 * children → kernelmanagerd spawns the matched dext.
 *
 * Actions:
 *   probe   — IOServiceRequestProbe on ConnectX (vendor 0x15b3)
 *   dump    — prints the node path + properties (diagnostics)
 *
 * Requires root (otherwise kIOReturnNotPrivileged). Built against the macOS SDK,
 * signed ad-hoc with iocatalog-management (notes/27: without the entitlement
 * IOCatalogue calls return fake-success; probe works just from root anyway,
 * but the signature doesn't hurt).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

/* private, exported on macOS */
extern kern_return_t IOServiceRequestProbe(io_object_t service, uint32_t options);

static int is_connectx(io_object_t svc)
{
    CFTypeRef vid = IORegistryEntrySearchCFProperty(svc, kIOServicePlane,
        CFSTR("vendor-id"), kCFAllocatorDefault, kIORegistryIterateRecursively);
    int hit = 0;
    if (vid && CFGetTypeID(vid) == CFDataGetTypeID() && CFDataGetLength(vid) >= 4) {
        const uint8_t *v = CFDataGetBytePtr(vid);
        /* vendor-id = 0x15b3 → LE bytes b3 15 */
        if (v[0] == 0xb3 && v[1] == 0x15) hit = 1;
    }
    if (vid) CFRelease(vid);
    return hit;
}

static int probe(void)
{
    CFMutableDictionaryRef match = IOServiceMatching("IOPCIDevice");
    io_iterator_t it;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &it);
    if (kr != KERN_SUCCESS) {
        printf("IOServiceGetMatchingServices(IOPCIDevice): 0x%x\n", kr);
        return 1;
    }
    int found = 0;
    io_object_t svc;
    while ((svc = IOIteratorNext(it)) != 0) {
        if (!is_connectx(svc)) { IOObjectRelease(svc); continue; }
        found = 1;
        char path[512];
        IORegistryEntryGetPath(svc, kIOServicePlane, path);
        kr = IOServiceRequestProbe(svc, 0);
        printf("IOServiceRequestProbe(%s): 0x%x\n", path, kr);
        /* also yank the parent IOPP node — sometimes the device sits deeper */
        io_registry_entry_t parent = 0;
        IORegistryEntryGetParentEntry(svc, kIOServicePlane, &parent);
        if (parent) {
            char ppath[512];
            IORegistryEntryGetPath(parent, kIOServicePlane, ppath);
            kern_return_t kr2 = IOServiceRequestProbe(parent, 0);
            printf("IOServiceRequestProbe(parent %s): 0x%x\n", ppath, kr2);
            IOObjectRelease(parent);
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    if (!found) { printf("ConnectX (vendor 0x15b3) not found in IOPCIDevice\n"); return 1; }
    return 0;
}

static int dump(void)
{
    CFMutableDictionaryRef match = IOServiceMatching("IOPCIDevice");
    io_iterator_t it;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &it);
    if (kr != KERN_SUCCESS) { printf("get matching services: 0x%x\n", kr); return 1; }
    io_object_t svc;
    while ((svc = IOIteratorNext(it)) != 0) {
        if (!is_connectx(svc)) { IOObjectRelease(svc); continue; }
        char path[512];
        IORegistryEntryGetPath(svc, kIOServicePlane, path);
        printf("ConnectX: %s\n", path);
        CFMutableDictionaryRef props = NULL;
        if (IORegistryEntryCreateCFProperties(svc, &props, kCFAllocatorDefault, 0)
                == KERN_SUCCESS && props) {
            CFShow(props);
            CFRelease(props);
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return 0;
}

int main(int argc, char **argv)
{
    const char *act = argc > 1 ? argv[1] : "probe";
    if (!strcmp(act, "dump")) return dump();
    if (!strcmp(act, "probe")) return probe();
    printf("usage: %s {probe|dump}\n", argv[0]);
    return 1;
}
