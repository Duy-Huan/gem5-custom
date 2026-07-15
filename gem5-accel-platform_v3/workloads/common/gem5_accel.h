/*
 * gem5_accel.h - userspace wrappers for the gem5_accel_* SE-mode syscalls.
 *
 * These are NOT real Linux syscalls - they only exist inside this gem5
 * simulation (see src/arch-riscv-patch/gem5_accel_syscall.inc.cc). Code
 * using this header will NOT run on real RISC-V hardware; it is only for
 * benchmarks executed under gem5 SE mode with the accelerator patch
 * applied.
 *
 * Register-passing convention matches the RISC-V Linux syscall ABI:
 * syscall number in a7, up to 6 args in a0-a5, return value in a0.
 */
#ifndef GEM5_ACCEL_H
#define GEM5_ACCEL_H

#include <stdint.h>

#define SYS_GEM5_ACCEL_START  2012
#define SYS_GEM5_ACCEL_POLL   2013
#define SYS_GEM5_ACCEL_CYCLES 2014

#define ACCEL_STATUS_BUSY  (1ULL << 0)
#define ACCEL_STATUS_DONE  (1ULL << 1)
#define ACCEL_STATUS_ERROR (1ULL << 2)

/* Assign these consistently with accel_id= in your gem5 config script. */
#define ACCEL_ID_NPU 0
#define ACCEL_ID_LPU 1
#define ACCEL_ID_ISP 2

static inline long
gem5_accel_syscall6(long n, long a0, long a1, long a2, long a3, long a4,
                     long a5)
{
    register long a7 __asm__("a7") = n;
    register long r0 __asm__("a0") = a0;
    register long r1 __asm__("a1") = a1;
    register long r2 __asm__("a2") = a2;
    register long r3 __asm__("a3") = a3;
    register long r4 __asm__("a4") = a4;
    register long r5 __asm__("a5") = a5;
    __asm__ volatile("ecall"
                      : "+r"(r0)
                      : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(a7)
                      : "memory");
    return r0;
}

/* Kick off an op. src/dst buffers must not cross a 4KiB page boundary
 * (v1 limitation - see gem5_accel_syscall.inc.cc). Returns 0 on success,
 * a negative errno-style value otherwise (-EBUSY, -EFAULT, -EINVAL). */
static inline long
gem5_accel_start(int accel_id, void *src, void *dst, uint64_t len_bytes,
                  uint64_t param0, uint64_t param1)
{
    return gem5_accel_syscall6(SYS_GEM5_ACCEL_START, accel_id,
                                (long)(uintptr_t)src, (long)(uintptr_t)dst,
                                (long)len_bytes, (long)param0, (long)param1);
}

/* Returns the STATUS register bits (ACCEL_STATUS_*). */
static inline long
gem5_accel_poll(int accel_id)
{
    return gem5_accel_syscall6(SYS_GEM5_ACCEL_POLL, accel_id, 0, 0, 0, 0, 0);
}

/* Returns the device-cycle count of the last completed op. */
static inline long
gem5_accel_cycles(int accel_id)
{
    return gem5_accel_syscall6(SYS_GEM5_ACCEL_CYCLES, accel_id, 0, 0, 0, 0,
                                0);
}

/* Busy-poll helper: blocks (spins) until DONE or ERROR is set. */
static inline long
gem5_accel_wait(int accel_id)
{
    long st;
    do {
        st = gem5_accel_poll(accel_id);
    } while (!(st & (ACCEL_STATUS_DONE | ACCEL_STATUS_ERROR)));
    return st;
}

/* Helper packers matching each accelerator's PARAM0/PARAM1 convention. */
static inline uint64_t
npu_pack_dims(uint16_t M, uint16_t K, uint16_t N)
{
    return ((uint64_t)M << 32) | ((uint64_t)K << 16) | (uint64_t)N;
}

static inline uint64_t
isp_pack_dims(uint16_t width, uint16_t height)
{
    return ((uint64_t)width << 16) | (uint64_t)height;
}

#endif /* GEM5_ACCEL_H */
