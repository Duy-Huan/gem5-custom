#ifndef __ACCELERATORS_NPU_ACCEL_RTL_HH__
#define __ACCELERATORS_NPU_ACCEL_RTL_HH__

#include "accelerators/accel_base.hh"
#include "params/NpuAccelRtl.hh"
#include "sim/eventq.hh"

// Forward-declare the Verilator-generated model class instead of including
// its header here, to keep this header lightweight for anything that just
// needs to know NpuAccelRtl exists (e.g. AccelBase::lookup() callers).
// The real include lives in npu_accel_rtl.cc.
class Vnpu_mac_core;

namespace gem5
{

/**
 * NpuAccelRtl: an NPU model whose COMPUTE phase is driven by a real
 * Verilated RTL core (rtl/npu_mac_core.v) instead of an analytical
 * formula, while reusing AccelBase's register file, FSM, and *real* DMA
 * FETCH/WRITEBACK phases unchanged.
 *
 * This is the RTL-cosimulation counterpart to NpuAccel (the analytical
 * systolic-array model) - see docs/RTL-INTEGRATION.md for the full
 * design writeup (why/how this works, from first principles) and
 * docs/INTEGRATION.md for how NpuAccel itself fits into the platform.
 *
 * Register usage: identical to AccelBase's generic map (see accel_base.hh)
 * with PARAM0 unused (the element count is derived from LEN_BYTES) and
 * PARAM1 unused - this RTL core only implements one operation (streaming
 * MAC), unlike NpuAccel's GEMM-shaped PARAM0 packing.
 *
 * Data format: LEN_BYTES worth of the fetched buffer is interpreted as
 * pairs of little-endian int16_t (activation, weight), i.e.
 * lenBytes / 4 elements. The 32-bit accumulated result is written back
 * as the first 4 bytes of the output buffer.
 */
class NpuAccelRtl : public AccelBase
{
  public:
    using Params = NpuAccelRtlParams;
    NpuAccelRtl(const Params &p);
    ~NpuAccelRtl();

  protected:
    // We override beginCompute() directly instead of computeLatency() -
    // see accel_base.hh's doc-comment on beginCompute() for why.
    void beginCompute() override;
    const char *accelName() const override { return "NpuAccelRtl"; }

  private:
    Vnpu_mac_core *rtl;

    uint64_t numElems;
    uint64_t feedIdx;
    uint64_t elapsedCycles;

    EventFunctionWrapper stepEvent;

    void stepCycle();
    void resetRtl();
};

} // namespace gem5

#endif // __ACCELERATORS_NPU_ACCEL_RTL_HH__
