#include "accelerators/isp_accel.hh"

#include <cmath>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/Accel.hh"

namespace gem5
{

IspAccel::IspAccel(const Params &p) :
    AccelBase(p),
    pixelsPerCycle(p.pixels_per_cycle),
    stageOverheadCycles(p.stage_overhead_cycles),
    utilFactor(p.util_factor)
{
    panic_if(pixelsPerCycle == 0, "IspAccel: pixels_per_cycle must be > 0");
    panic_if(utilFactor <= 0.0 || utilFactor > 1.0,
             "IspAccel: util_factor must be in (0, 1], got %f", utilFactor);
}

Cycles
IspAccel::computeLatency(uint64_t lenBytes, uint64_t param0,
                          uint64_t param1) const
{
    const uint64_t width = (param0 >> 16) & 0xFFFF;
    const uint64_t height = param0 & 0xFFFF;
    const uint64_t numStages = param1;

    panic_if(width == 0 || height == 0 || numStages == 0,
             "IspAccel: PARAM0 must encode non-zero (width<<16)|height and "
             "PARAM1 must encode a non-zero num_stages");

    const uint64_t totalPixels = width * height;
    const uint64_t streamCycles =
        (totalPixels + pixelsPerCycle - 1) / pixelsPerCycle; // ceil div
    const uint64_t pipelineCost = numStages * stageOverheadCycles;
    const uint64_t rawCycles = streamCycles + pipelineCost;

    const uint64_t cycles = static_cast<uint64_t>(
        std::ceil(static_cast<double>(rawCycles) / utilFactor));

    DPRINTF(Accel,
            "IspAccel: %dx%d frame, %d px/cycle -> %d stream cycles, "
            "%d stages x %d cycles/stage = %d pipeline overhead, "
            "%d raw cycles, %d after util_factor=%.2f\n",
            width, height, pixelsPerCycle, streamCycles, numStages,
            stageOverheadCycles, pipelineCost, rawCycles, cycles,
            utilFactor);
    return Cycles(cycles);
}

} // namespace gem5
