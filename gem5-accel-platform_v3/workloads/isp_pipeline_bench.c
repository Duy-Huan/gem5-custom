/*
 * isp_pipeline_bench.c - drives the ISP streaming pipeline accelerator
 * through a series of (width, height, num_stages) frame calls.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common/gem5_accel.h"

struct isp_call
{
    uint16_t width, height;
    uint32_t num_stages;
};

int
main(void)
{
    static const struct isp_call calls[] = {
        {32, 32, 4},
        {48, 32, 6},
        {64, 16, 5},
    };
    const int numCalls = sizeof(calls) / sizeof(calls[0]);

    static uint8_t src_buf[4096] __attribute__((aligned(4096)));
    static uint8_t dst_buf[4096] __attribute__((aligned(4096)));

    printf("isp_pipeline_bench: %d frame calls\n", numCalls);

    uint64_t totalCycles = 0;
    for (int i = 0; i < numCalls; i++) {
        const struct isp_call *c = &calls[i];
        uint64_t bytes = (uint64_t)c->width * c->height; /* 1 byte/px demo */
        if (bytes > sizeof(src_buf))
            bytes = sizeof(src_buf);

        memset(src_buf, 0x11, bytes);

        long rc = gem5_accel_start(ACCEL_ID_ISP, src_buf, dst_buf, bytes,
                                    isp_pack_dims(c->width, c->height),
                                    c->num_stages);
        if (rc != 0) {
            printf("  call %d: gem5_accel_start failed rc=%ld\n", i, rc);
            continue;
        }

        long status = gem5_accel_wait(ACCEL_ID_ISP);
        long cycles = gem5_accel_cycles(ACCEL_ID_ISP);
        totalCycles += (uint64_t)cycles;

        printf("  call %d: %ux%u stages=%u bytes=%llu status=%#lx "
               "cycles=%ld\n",
               i, c->width, c->height, c->num_stages,
               (unsigned long long)bytes, status, cycles);
    }

    printf("isp_pipeline_bench: total device cycles = %llu\n",
           (unsigned long long)totalCycles);
    return 0;
}
