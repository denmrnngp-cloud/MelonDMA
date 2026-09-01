/* MlxServiceMatch.h — exact userspace matching for the MlxRDMA DEXT.
 *
 * A DriverKit implementation is published as the kernel class IOUserService.
 * IOUserClass is a registry property, not a top-level IOService matching key;
 * it therefore has to live under IOPropertyMatch.  Putting it directly in the
 * dictionary returned by IOServiceMatching("IOUserService") is silently
 * ignored and selects an unrelated DriverKit service.
 */
#ifndef MLX_SERVICE_MATCH_H
#define MLX_SERVICE_MATCH_H

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#define MLX_SERVICE_KERNEL_CLASS "IOUserService"
#define MLX_SERVICE_USER_CLASS   "MlxPCIDriver"
#define MLX_SERVICE_BUNDLE_ID    "com.mlx5.rdma.dext"

static inline CFMutableDictionaryRef
mlxCreateServiceMatching(void)
{
    CFMutableDictionaryRef matching =
        IOServiceMatching(MLX_SERVICE_KERNEL_CLASS);
    if (!matching) return NULL;

    CFMutableDictionaryRef properties = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!properties) {
        CFRelease(matching);
        return NULL;
    }

    CFDictionarySetValue(properties, CFSTR("IOUserClass"),
                         CFSTR(MLX_SERVICE_USER_CLASS));
    CFDictionarySetValue(properties, CFSTR("CFBundleIdentifier"),
                         CFSTR(MLX_SERVICE_BUNDLE_ID));
    CFDictionarySetValue(matching, CFSTR("IOPropertyMatch"), properties);
    CFRelease(properties);
    return matching;
}

#endif /* MLX_SERVICE_MATCH_H */
