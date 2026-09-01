#include <infiniband/verbs.h>

#include <stdio.h>

static int failures;

static void check(int condition, const char *message)
{
    if (condition) printf("ok:   %s\n", message);
    else { printf("FAIL: %s\n", message); failures++; }
}

int main(void)
{
    int count = -1;
    struct ibv_device **devices = ibv_get_device_list(&count);
    check(count >= 0, "device enumeration returns a non-negative count");
    check((count == 0 && devices == NULL) || (count > 0 && devices != NULL),
          "device enumeration result is internally consistent");
    ibv_free_device_list(devices);

    check(ibv_open_device(NULL) == NULL, "open_device(NULL) guarded");
    check(ibv_query_device(NULL, NULL) != 0, "query_device(NULL) guarded");
    check(ibv_mlx5_configure_roce(NULL, NULL) != 0,
          "configure_roce(NULL) guarded");
    check(ibv_alloc_pd(NULL) == NULL, "alloc_pd(NULL) guarded");
    check(ibv_create_cq(NULL, 1, NULL, NULL, 0) == NULL,
          "create_cq(NULL) guarded");
    check(ibv_create_comp_channel(NULL) == NULL,
          "create_comp_channel(NULL) guarded");
    check(ibv_req_notify_cq(NULL, 0) != 0,
          "req_notify_cq(NULL) guarded");
    check(ibv_get_cq_event(NULL, NULL, NULL) != 0,
          "get_cq_event(NULL) guarded");
    ibv_ack_cq_events(NULL, 1);
    check(1, "ack_cq_events(NULL) guarded");
    check(ibv_create_ah(NULL, NULL) == NULL, "create_ah(NULL) guarded");
    check(ibv_create_qp(NULL, NULL) == NULL, "create_qp(NULL) guarded");
    check(ibv_query_qp(NULL, NULL, 0, NULL) != 0, "query_qp(NULL) guarded");
    check(ibv_get_async_event(NULL, NULL) != 0, "async_event(NULL) guarded");
    check(ibv_reg_mr(NULL, NULL, 0, 0) == NULL, "reg_mr(NULL) guarded");
    check(ibv_poll_cq(NULL, 1, NULL) < 0, "poll_cq(NULL) guarded");
    check(ibv_post_send(NULL, NULL, NULL) != 0, "post_send(NULL) guarded");
    check(ibv_post_recv(NULL, NULL, NULL) != 0, "post_recv(NULL) guarded");
    check(ibv_wc_status_str(IBV_WC_SUCCESS) != NULL,
          "completion status strings available");

    if (failures) {
        printf("\n%d ibverbs compatibility test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nALL ibverbs compatibility smoke tests passed\n");
    return 0;
}
