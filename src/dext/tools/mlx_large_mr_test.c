/* Decisive large-MR test: does ibv_reg_mr split+indirect actually work
 * against the live DEXT, and how expensive is the registration?
 * Usage: mlx_large_mr_test [size_bytes]  (default 10 MiB)
 */
#include <infiniband/verbs.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    size_t size = argc > 1 ? (size_t)strtoull(argv[1], 0, 0) : (10u << 20);
    struct ibv_device **devs = ibv_get_device_list(NULL);
    if (!devs || !devs[0]) { fprintf(stderr, "no devices\n"); return 1; }
    printf("device: %s\n", ibv_get_device_name(devs[0]));
    struct ibv_context *ctx = ibv_open_device(devs[0]);
    if (!ctx) { perror("ibv_open_device"); return 1; }
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) { perror("ibv_alloc_pd"); return 1; }
    void *buf = malloc(size);
    if (!buf) { perror("malloc"); return 1; }
    memset(buf, 0xAB, size);

    double t0 = now_s();
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, size,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    double dt = now_s() - t0;
    if (!mr) {
        fprintf(stderr, "ibv_reg_mr FAILED size=%zu MiB errno=%d (%s)\n",
                size >> 20, errno, strerror(errno));
        return 1;
    }
    printf("ibv_reg_mr OK: size=%zu MiB lkey=0x%x rkey=0x%x time=%.1f ms (%.1f MiB/s)\n",
           size >> 20, mr->lkey, mr->rkey, dt * 1000.0, (size >> 20) / dt);

    double t1 = now_s();
    int rc = ibv_dereg_mr(mr);
    printf("ibv_dereg_mr rc=%d time=%.1f ms\n", rc, (now_s() - t1) * 1000.0);

    free(buf);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    ibv_free_device_list(devs);
    return 0;
}
