/* mlx_reinit.c — restart firmware init of the MlxRDMA dext WITHOUT
 * kill/rematch (kMlxUCMethodFwReinit via MlxUserClient, notes/31).
 * The card owner does not change → IOPCIFamily does NOT do its FLR →
 * the card does not go into a long reset.
 *
 * Build: cc -o build/mlx_reinit tools/mlx_reinit.c -framework IOKit -framework CoreFoundation
 * Run: ./build/mlx_reinit
 */
#include <stdio.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include "MlxServiceMatch.h"

/* must match Sources/userclient/MlxUCIO.h */
#define MLX_UC_SELECTOR_FWREINIT 0x10A0
int main(void)
{
    io_service_t svc = IOServiceGetMatchingService(
        kIOMainPortDefault, mlxCreateServiceMatching());
    if (!svc) {
        fprintf(stderr, "MlxPCIDriver not found — card is not with our dext\n");
        return 1;
    }
    printf("MlxPCIDriver found (handle 0x%x)\n", svc);

    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "IOServiceOpen: 0x%x\n", kr);
        return 1;
    }
    printf("UserClient opened, calling FwReinit (takes up to ~60s: FLR + fw load)...\n");

    kr = IOConnectCallMethod(conn, MLX_UC_SELECTOR_FWREINIT,
                             NULL, 0,                 /* scalar in */
                             NULL, 0,                 /* struct in */
                             NULL, NULL,              /* scalar out */
                             NULL, NULL);             /* struct out */
    if (kr == kIOReturnSuccess)
        printf("SUCCESS: firmware init restarted (see kernel log)\n");
    else
        fprintf(stderr, "FwReinit: 0x%x (kr)\n", kr);

    IOServiceClose(conn);
    return kr == kIOReturnSuccess ? 0 : 1;
}
