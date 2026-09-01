/* Regression test for the DriverKit registry matching dictionary. */
#include "MlxServiceMatch.h"
#include <stdio.h>

static int failures;

static void check(int condition, const char *message)
{
    if (condition) printf("ok:   %s\n", message);
    else { printf("FAIL: %s\n", message); failures++; }
}

int main(void)
{
    CFMutableDictionaryRef matching = mlxCreateServiceMatching();
    check(matching != NULL, "service matching dictionary created");
    if (!matching) return 1;

    check(!CFDictionaryContainsKey(matching, CFSTR("IOUserClass")),
          "IOUserClass is not an ignored top-level matching key");
    CFTypeRef value = CFDictionaryGetValue(matching,
                                           CFSTR("IOPropertyMatch"));
    check(value && CFGetTypeID(value) == CFDictionaryGetTypeID(),
          "IOPropertyMatch dictionary present");
    if (value && CFGetTypeID(value) == CFDictionaryGetTypeID()) {
        CFDictionaryRef properties = (CFDictionaryRef)value;
        CFTypeRef userClass = CFDictionaryGetValue(properties,
                                                    CFSTR("IOUserClass"));
        CFTypeRef bundleId = CFDictionaryGetValue(properties,
                                                   CFSTR("CFBundleIdentifier"));
        check(userClass && CFEqual(userClass, CFSTR(MLX_SERVICE_USER_CLASS)),
              "IOPropertyMatch selects MlxPCIDriver");
        check(bundleId && CFEqual(bundleId, CFSTR(MLX_SERVICE_BUNDLE_ID)),
              "IOPropertyMatch selects the MlxRDMA bundle");
    }
    CFRelease(matching);

    if (failures) {
        printf("\n%d service matching test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nALL service matching tests passed\n");
    return 0;
}
