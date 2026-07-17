#include "accelerators/npu_accel_rtl.hh"

#include <cstdint>
#include <cstring>

#include "Vnpu_mac_core.h"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/Accel.hh"

namespace gem5
{

NpuAccelRtl::NpuAccelRtl(const Params &p) :
    AccelBase(p),
    rtl(new Vnpu_mac_core),
    numElems(0), feedIdx(0), elapsedCycles(0),
    stepEvent([this]{ stepCycle(); }, name() + ".stepEvent")
{
    resetRtl();
}

NpuAccelRtl::~NpuAccelRtl()
{
    rtl->final();
    delete rtl;
}

void
NpuAccelRtl::resetRtl()
{
    // Verilator models have no implicit initial state guarantee for
    // registers until an explicit reset pulse has been applied - always
    // do this before the first operation, exactly as you would on real
    // silicon after power-up.
    rtl->rst_n = 0;
    rtl->start = 0;
    rtl->in_valid = 0;
    rtl->in_a = 0;
    rtl->in_b = 0;

    for (int i = 0; i < 2; i++) {
        rtl->clk = 0; rtl->eval();
        rtl->clk = 1; rtl->eval();
    }
    rtl->rst_n = 1;
    rtl->clk = 0; rtl->eval();
    rtl->clk = 1; rtl->eval();
}

void
NpuAccelRtl::beginCompute()
{
    // LEN_BYTES worth of the fetched buffer, interpreted as pairs of
    // int16_t (activation, weight) - see the class doc-comment.
    numElems = scratchLen() / (2 * sizeof(int16_t));
    feedIdx = 0;
    elapsedCycles = 0;

    panic_if(numElems == 0 || numElems > 0xFFFF,
             "%s: element count %d (from LEN_BYTES=%d) out of RTL's "
             "16-bit length range", accelName(), numElems, scratchLen());

    rtl->start = 1;
    rtl->length = static_cast<uint16_t>(numElems);
    rtl->clk = 0; rtl->eval();
    rtl->clk = 1; rtl->eval();
    rtl->start = 0;

    DPRINTF(Accel, "%s: RTL start, %d elements\n", accelName(), numElems);
    schedule(stepEvent, curTick() + clockPeriod());
}

void
NpuAccelRtl::stepCycle()
{
    const int16_t *data =
        reinterpret_cast<const int16_t *>(scratchPtr());

    if (feedIdx < numElems) {
        rtl->in_valid = 1;
        rtl->in_a = static_cast<uint16_t>(data[2 * feedIdx]);
        rtl->in_b = static_cast<uint16_t>(data[2 * feedIdx + 1]);
    } else {
        rtl->in_valid = 0;
    }

    // One full clock period = both edges, matching how the standalone
    // Verilator testbench (test/standalone_test.cpp) drove the same RTL
    // during pre-integration verification - see RTL-INTEGRATION.md.
    rtl->clk = 0; rtl->eval();
    rtl->clk = 1; rtl->eval();
    elapsedCycles++;

    if (rtl->in_valid && rtl->in_ready)
        feedIdx++;

    if (rtl->done) {
        int32_t result = static_cast<int32_t>(rtl->result);
        std::memcpy(scratchPtr(), &result, sizeof(result));

        DPRINTF(Accel, "%s: RTL done after %d real cycles, result=%d\n",
                accelName(), elapsedCycles, result);

        // NOTE: we do not set cyclesLastReg here - AccelBase::
        // onWritebackDone() already derives it from the real elapsed
        // simulation ticks (curTick() - opStartTick), which is correct
        // and automatically includes every cycle we just spent stepping
        // the RTL via real schedule() calls. elapsedCycles above is kept
        // only for the DPRINTF message / sanity cross-checking.
        finishCompute();
    } else {
        schedule(stepEvent, curTick() + clockPeriod());
    }
}

} // namespace gem5
