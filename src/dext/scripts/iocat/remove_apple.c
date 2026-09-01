#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
int main(int argc, char **argv) {
    const char *bid = (argc > 1) ? argv[1] : "com.apple.DriverKit-AppleEthernetMLX5";
    CFMutableDictionaryRef p = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(p, CFSTR("CFBundleIdentifier"), CFStringCreateWithCString(NULL, bid, kCFStringEncodingUTF8));
    CFDataRef data = CFPropertyListCreateData(NULL, p, kCFPropertyListXMLFormat_v1_0, 0, NULL);
    CFIndex len = CFDataGetLength(data);
    char *buf = malloc(len + 1);
    memcpy(buf, CFDataGetBytePtr(data), len);
    buf[len] = 0;
    kern_return_t kr = IOCatalogueSendData(kIOMainPortDefault, 3, buf, (uint32_t)(len + 1));
    printf("remove(%s): 0x%x\n", bid, kr);
    return 0;
}
