/*
 * AccelBase - common infrastructure for company accelerator models
 * (NPU / LPU / ISP) built on top of gem5's real DmaDevice + PioDevice
 * infrastructure so that:
 *   - the control-plane (registers) behaves like a real memory-mapped
 *     peripheral (works unmodified in Full-System mode),
 *   - the data-plane (DMA transfers) is issued as real timing Packets on
 *     gem5's port/interconnect model, so bus throughput/contention and
 *     memory latency are captured for real in gem5 stats,
 *   - compute latency is modeled at cycle granularity using the device's
 *     own clock domain (ClockedObject).
 *
 * See docs/ARCHITECTURE.md in this package for the full design writeup,
 * and docs/INTEGRATION.md for exactly where to drop these files into a
 * gem5 checkout.
 */

#ifndef __ACCELERATORS_ACCEL_BASE_HH__
#define __ACCELERATORS_ACCEL_BASE_HH__

#include <memory>
#include <unordered_map>

#include "base/statistics.hh"
#include "dev/dma_device.hh"
#include "params/AccelBase.hh"
#include "sim/eventq.hh"

namespace gem5
{

/**
 * Common register-file + DMA + FSM base class for a memory-mapped
 * accelerator. Subclasses (NpuAccel, LpuAccel, IspAccel, ...) only need to
 * implement computeLatency() with their own timing model.
 *
 * Register map (all registers are 8 bytes, little-endian, offsets relative
 * to pio_addr):
 *
 *   0x00  CTRL         (W)  bit0 = START, bit1 = (reserved) SOFT_RESET
 *   0x08  STATUS       (R/W) bit0 = BUSY, bit1 = DONE, bit2 = ERROR
 *                            (write 0 to acknowledge/clear DONE|ERROR)
 *   0x10  SRC_ADDR     (R/W) physical address of the input buffer
 *   0x18  DST_ADDR     (R/W) physical address of the output buffer
 *   0x20  LEN_BYTES    (R/W) number of bytes to DMA in and out
 *   0x28  PARAM0       (R/W) accelerator-specific parameter word #0
 *   0x30  PARAM1       (R/W) accelerator-specific parameter word #1
 *   0x38  CYCLES_LAST  (R)  cycle count consumed by the last completed op
 *
 * Software contract: write SRC_ADDR/DST_ADDR/LEN_BYTES/PARAM0/PARAM1 first,
 * then write CTRL.START=1. Poll STATUS until DONE (or ERROR) is set, read
 * CYCLES_LAST if desired, then write STATUS=0 to acknowledge before
 * starting the next op.
 *
 * In Syscall-Emulation (SE) mode there is no OS to map this MMIO window
 * into a process's virtual address space, so the same functionality is
 * also reachable through the `gem5_accel` SE syscall shim (see
 * docs/INTEGRATION.md + workloads/common/gem5_accel.h), which calls
 * startOp()/pollStatus()/lastCycles() directly after translating the
 * caller's virtual buffer pointers to physical addresses. The DMA/compute
 * timing path is identical either way.
 */
class AccelBase : public DmaDevice
{
  public:
    enum RegOffset : Addr
    {
        REG_CTRL        = 0x00,
        REG_STATUS      = 0x08,
        REG_SRC_ADDR    = 0x10,
        REG_DST_ADDR    = 0x18,
        REG_LEN         = 0x20,
        REG_PARAM0      = 0x28,
        REG_PARAM1      = 0x30,
        REG_CYCLES_LAST = 0x38,
    };
    static const Addr RegWindowSize = 0x40;

    enum CtrlBits : uint64_t
    {
        CTRL_START = 1ULL << 0,
        CTRL_SOFT_RESET = 1ULL << 1,
    };

    enum StatusBits : uint64_t
    {
        STATUS_BUSY  = 1ULL << 0,
        STATUS_DONE  = 1ULL << 1,
        STATUS_ERROR = 1ULL << 2,
    };

    using Params = AccelBaseParams;
    AccelBase(const Params &p);
    virtual ~AccelBase() = default;

    // PioDevice interface (MMIO path; used as-is in Full-System mode).
    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
    AddrRangeList getAddrRanges() const override;

    // SE-mode syscall-shim entry points. Addresses passed here must
    // already be *physical* addresses (the syscall shim translates the
    // caller's virtual pointers before calling in).
    uint64_t startOp(Addr srcPaddr, Addr dstPaddr, uint64_t lenBytes,
                      uint64_t param0, uint64_t param1);
    uint64_t pollStatus() const { return statusReg; }
    uint64_t lastCycles() const { return cyclesLastReg; }

    /** Looks up a registered accelerator by the `accel_id` Python param.
     *  Used by the gem5_accel SE syscall to dispatch to the right device.
     */
    static AccelBase *lookup(int accelId);

  protected:
    /**
     * Subclasses implement their own timing model here. Called once the
     * input buffer has been fully fetched (DMA read complete). Return the
     * number of *device clock cycles* the compute stage should take.
     */
    virtual Cycles computeLatency(uint64_t lenBytes, uint64_t param0,
                                   uint64_t param1) const = 0;

    /** Short device name used in DPRINTF messages. */
    virtual const char *accelName() const = 0;

    const int accelId;

  private:
    enum class State { Idle, Fetch, Compute, Writeback };
    State state;

    uint64_t ctrlReg;
    uint64_t statusReg;
    uint64_t srcAddrReg;
    uint64_t dstAddrReg;
    uint64_t lenReg;
    uint64_t param0Reg;
    uint64_t param1Reg;
    uint64_t cyclesLastReg;

    Tick opStartTick;
    std::unique_ptr<uint8_t[]> scratch;

    EventFunctionWrapper fetchDoneEvent;
    EventFunctionWrapper computeDoneEvent;
    EventFunctionWrapper writebackDoneEvent;

    void beginOp();
    void onFetchDone();
    void onComputeDone();
    void onWritebackDone();

    static std::unordered_map<int, AccelBase *> registry;

    struct AccelStats : public statistics::Group
    {
        AccelStats(AccelBase &a);
        statistics::Scalar opsCompleted;
        statistics::Scalar opsFailed;
        statistics::Scalar bytesTransferred;
        statistics::Scalar busyCycles;
    } stats;
};

} // namespace gem5

#endif // __ACCELERATORS_ACCEL_BASE_HH__
