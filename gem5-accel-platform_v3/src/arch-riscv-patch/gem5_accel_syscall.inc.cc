// ---------------------------------------------------------------------------
// gem5_accel SE-mode syscall shim
// ---------------------------------------------------------------------------
// This file is NOT compiled on its own - it exists so you have a clean,
// reviewable copy of the three functions to paste into
// src/arch/riscv/linux/se_workload.cc in your gem5 checkout. See
// docs/INTEGRATION.md for exact anchor points (search for "getmainvars").
//
// WHY A SYSCALL SHIM AT ALL?
// In Syscall-Emulation (SE) mode there is no kernel/driver stack, so a
// user process has no standard way to get a device's MMIO window mapped
// into its virtual address space. Real gem5-based accelerator research
// projects (e.g. gem5-Aladdin) hit the same problem and solve it the same
// way: add a small number of SE-mode syscalls (or pseudo-instructions)
// that act as the "driver" for the control-plane, while the actual DMA
// data movement and compute timing still go through gem5's real
// port/event/clock infrastructure - so bus throughput, memory latency and
// compute cycles are still modeled for real. Only the "how does software
// tell the device to start" step is simplified.
//
// If/when you move to Full-System (FS) mode with a real Linux image, you
// do NOT need this shim at all: mmap() the accelerator's physical MMIO
// window (via /dev/mem or a real kernel driver) and read/write the
// registers directly - AccelBase::read()/write() already implement that
// path using gem5's standard PioDevice mechanism.
//
// KNOWN LIMITATION (v1): srcVaddr/dstVaddr buffers must not cross a page
// boundary (<= 4KiB and page-aligned-contiguous), because SE-mode virtual
// pages are not guaranteed contiguous in simulated physical memory across
// page boundaries, and this shim only translates the starting address.
// For larger buffers, either (a) have your benchmark issue one
// gem5_accel_start() call per 4KiB page and let the FSM run back-to-back,
// or (b) extend AccelBase to accept a scatter/gather list of
// (vaddr,len) chunks and issue one dmaRead/dmaWrite per chunk internally.

#include <cerrno>

#include "accelerators/accel_base.hh"
#include "mem/page_table.hh"
#include "sim/process.hh"
#include "sim/syscall_desc.hh"
#include "sim/system.hh"

namespace gem5
{

namespace
{

constexpr Addr GEM5_ACCEL_PAGE_SIZE = 0x1000; // 4 KiB, matches RISC-V default

bool
translateSingleBuffer(ThreadContext *tc, Addr vaddr, uint64_t len,
                       Addr &paddr)
{
    if (len == 0)
        return false;

    const Addr firstPage = vaddr & ~(GEM5_ACCEL_PAGE_SIZE - 1);
    const Addr lastPage = (vaddr + len - 1) & ~(GEM5_ACCEL_PAGE_SIZE - 1);
    if (firstPage != lastPage)
        return false; // crosses a page boundary - see limitation above

    return tc->getProcessPtr()->pTable->translate(vaddr, paddr);
}

} // anonymous namespace

/// SYS_gem5_accel_start (paste-in syscall number, e.g. 2012)
/// args: accel_id, src_vaddr, dst_vaddr, len_bytes, param0, param1
/// returns: 0 on success, -EINVAL (bad accel_id), -EFAULT (bad/unaligned
///          buffer or crosses a page boundary), -EBUSY (device busy)
SyscallReturn
gem5AccelStartFunc(SyscallDesc *desc, ThreadContext *tc, int accelId,
                    Addr srcVaddr, Addr dstVaddr, uint64_t lenBytes,
                    uint64_t param0, uint64_t param1)
{
    AccelBase *dev = AccelBase::lookup(accelId);
    if (!dev)
        return -EINVAL;

    Addr srcPaddr, dstPaddr;
    if (!translateSingleBuffer(tc, srcVaddr, lenBytes, srcPaddr))
        return -EFAULT;
    if (!translateSingleBuffer(tc, dstVaddr, lenBytes, dstPaddr))
        return -EFAULT;

    uint64_t rc = dev->startOp(srcPaddr, dstPaddr, lenBytes, param0, param1);
    return rc == 0 ? 0 : -EBUSY;
}

/// SYS_gem5_accel_poll (paste-in syscall number, e.g. 2013)
/// args: accel_id
/// returns: STATUS register bits (AccelBase::StatusBits), or -EINVAL
SyscallReturn
gem5AccelPollFunc(SyscallDesc *desc, ThreadContext *tc, int accelId)
{
    AccelBase *dev = AccelBase::lookup(accelId);
    if (!dev)
        return -EINVAL;
    return static_cast<int64_t>(dev->pollStatus());
}

/// SYS_gem5_accel_cycles (paste-in syscall number, e.g. 2014)
/// args: accel_id
/// returns: cycle count of the last completed op, or -EINVAL
SyscallReturn
gem5AccelCyclesFunc(SyscallDesc *desc, ThreadContext *tc, int accelId)
{
    AccelBase *dev = AccelBase::lookup(accelId);
    if (!dev)
        return -EINVAL;
    return static_cast<int64_t>(dev->lastCycles());
}

} // namespace gem5

// ---------------------------------------------------------------------------
// And add these three lines inside EmuLinux::syscallDescs64 in the same
// file, right after the existing {2011, "getmainvars"} entry:
//
//     {2012, "gem5_accel_start", gem5AccelStartFunc},
//     {2013, "gem5_accel_poll", gem5AccelPollFunc},
//     {2014, "gem5_accel_cycles", gem5AccelCyclesFunc},
// ---------------------------------------------------------------------------
