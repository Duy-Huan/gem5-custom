/*
 * npu_matmul_bench.c - drives the NPU accelerator model through a series
 * of GEMM (M x K x N) calls and reports achieved throughput.
 *
 * Build: see workloads/Makefile (RISC-V cross toolchain required).
 * Run:   see configs/run_benchmark.py
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/gem5_accel.h"

/* One GEMM call per entry: M, K, N (all <= a few hundred so the input
 * blob stays within one 4KiB page under the v1 single-page limitation). */
struct gemm_call
{
    uint16_t M, K, N;
};

int
main(void)
{
    static const struct gemm_call calls[] = {
        {8, 8, 8},
        {16, 16, 16},
        {8, 32, 8},
        {4, 64, 4},
    };
    const int numCalls = sizeof(calls) / sizeof(calls[0]);

    /* Bounce buffers - kept small and page-aligned so a single call never
     * crosses a page boundary (v1 shim limitation). */
    static uint8_t src_buf[4096] __attribute__((aligned(4096)));
    static uint8_t dst_buf[4096] __attribute__((aligned(4096)));

    printf("npu_matmul_bench: %d GEMM calls\n", numCalls);

    uint64_t totalCycles = 0;
    for (int i = 0; i < numCalls; i++) {
        const struct gemm_call *c = &calls[i];
        uint64_t bytes = (uint64_t)c->M * c->K + (uint64_t)c->K * c->N;
        if (bytes > sizeof(src_buf))
            bytes = sizeof(src_buf); /* clamp for this simple demo */

        memset(src_buf, 0xA5, bytes);

        long rc = gem5_accel_start(ACCEL_ID_NPU, src_buf, dst_buf, bytes,
                                    npu_pack_dims(c->M, c->K, c->N), 0);
        if (rc != 0) {
            printf("  call %d: gem5_accel_start failed rc=%ld\n", i, rc);
            continue;
        }

        long status = gem5_accel_wait(ACCEL_ID_NPU);
        long cycles = gem5_accel_cycles(ACCEL_ID_NPU);
        totalCycles += (uint64_t)cycles;

        printf("  call %d: M=%u K=%u N=%u bytes=%llu status=%#lx "
               "cycles=%ld\n",
               i, c->M, c->K, c->N, (unsigned long long)bytes, status,
               cycles);
    }

    printf("npu_matmul_bench: total device cycles = %llu\n",
           (unsigned long long)totalCycles);
    return 0;
}
