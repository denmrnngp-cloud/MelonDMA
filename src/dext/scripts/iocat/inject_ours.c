#include <stdio.h>
#include <string.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
int main(int argc, char **argv) {
    int score = (argc > 1) ? atoi(argv[1]) : 5000;
    CFMutableDictionaryRef p = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(p, CFSTR("CFBundleIdentifier"), CFSTR("com.mlx5.rdma.dext"));
    CFDictionarySetValue(p, CFSTR("CFBundleIdentifierKernel"), CFSTR("com.apple.kpi.iokit"));
    CFDictionarySetValue(p, CFSTR("IOClass"), CFSTR("IOUserService"));
    CFDictionarySetValue(p, CFSTR("IOProviderClass"), CFSTR("IOPCIDevice"));
    CFDictionarySetValue(p, CFSTR("IOPCIMatch"), CFSTR("0x101515b3"));
    CFDictionarySetValue(p, CFSTR("IOPCITunnelCompatible"), kCFBooleanTrue);
    CFDictionarySetValue(p, CFSTR("IOUserServerName"), CFSTR("com.mlx5.rdma.dext"));
    CFDictionarySetValue(p, CFSTR("IOUserClass"), CFSTR("MlxPCIDriver"));
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &score);
    CFDictionarySetValue(p, CFSTR("IOProbeScore"), n);
    CFRelease(n);
    CFArrayRef arr = CFArrayCreate(NULL, (const void**)&p, 1, &kCFTypeArrayCallBacks);
    CFDataRef data = CFPropertyListCreateData(NULL, arr, kCFPropertyListXMLFormat_v1_0, 0, NULL);
    CFIndex len = CFDataGetLength(data);
    // KEY FIX: copy with a NUL terminator, length = len+1
    char *buf = malloc(len + 1);
    memcpy(buf, CFDataGetBytePtr(data), len);
    buf[len] = 0;
    kern_return_t kr = IOCatalogueSendData(kIOMainPortDefault, 1, buf, (uint32_t)(len + 1));
    printf("inject(score=%d, len=%ld+1): 0x%x\n", score, (long)len, kr);
    return 0;
}
