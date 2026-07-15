/*
 * lpu_stream_bench.c - drives the LPU streaming/dataflow accelerator
 * through a series of (seq_len, model_dim) token-batch calls.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common/gem5_accel.h"

struct lpu_call
{
    uint32_t seq_len;
    uint32_t model_dim;
};

int
main(void)
{
    static const struct lpu_call calls[] = {
        {8, 64},
        {16, 64},
        {32, 32},
    };
    const int numCalls = sizeof(calls) / sizeof(calls[0]);

    static uint8_t src_buf[4096] __attribute__((aligned(4096)));
    static uint8_t dst_buf[4096] __attribute__((aligned(4096)));

    printf("lpu_stream_bench: %d calls\n", numCalls);

    uint64_t totalCycles = 0;
    for (int i = 0; i < numCalls; i++) {
        const struct lpu_call *c = &calls[i];
        uint64_t bytes = (uint64_t)c->seq_len * c->model_dim;
        if (bytes > sizeof(src_buf))
            bytes = sizeof(src_buf);

        memset(src_buf, 0x5A, bytes);

        long rc = gem5_accel_start(ACCEL_ID_LPU, src_buf, dst_buf, bytes,
                                    c->seq_len, c->model_dim);
        if (rc != 0) {
            printf("  call %d: gem5_accel_start failed rc=%ld\n", i, rc);
            continue;
        }

        long status = gem5_accel_wait(ACCEL_ID_LPU);
        long cycles = gem5_accel_cycles(ACCEL_ID_LPU);
        totalCycles += (uint64_t)cycles;

        printf("  call %d: seq_len=%u model_dim=%u bytes=%llu status=%#lx "
               "cycles=%ld\n",
               i, c->seq_len, c->model_dim, (unsigned long long)bytes,
               status, cycles);
    }

    printf("lpu_stream_bench: total device cycles = %llu\n",
           (unsigned long long)totalCycles);
    return 0;
}
