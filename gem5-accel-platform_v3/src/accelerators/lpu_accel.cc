#include "accelerators/lpu_accel.hh"

#include <cmath>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/Accel.hh"

namespace gem5
{

LpuAccel::LpuAccel(const Params &p) :
    AccelBase(p),
    lanesPerCycle(p.lanes_per_cycle),
    pipelineDepth(p.pipeline_depth),
    setupCycles(p.setup_cycles),
    utilFactor(p.util_factor)
{
    panic_if(lanesPerCycle == 0, "LpuAccel: lanes_per_cycle must be > 0");
    panic_if(utilFactor <= 0.0 || utilFactor > 1.0,
             "LpuAccel: util_factor must be in (0, 1], got %f", utilFactor);
}

Cycles
LpuAccel::computeLatency(uint64_t lenBytes, uint64_t param0,
                          uint64_t param1) const
{
    const uint64_t seqLen = param0;
    const uint64_t modelDim = param1;

    panic_if(seqLen == 0 || modelDim == 0,
             "LpuAccel: PARAM0 (seq_len) and PARAM1 (model_dim) must be "
             "non-zero");

    const uint64_t cyclesPerToken =
        (modelDim + lanesPerCycle - 1) / lanesPerCycle; // ceil div
    const uint64_t rawCycles =
        setupCycles + seqLen * cyclesPerToken + pipelineDepth;
    const uint64_t cycles = static_cast<uint64_t>(
        std::ceil(static_cast<double>(rawCycles) / utilFactor));

    // How much of the run was "useful streaming" vs fixed per-call
    // overhead - useful when tuning batch sizes against setup_cycles.
    const double amortizedPct =
        rawCycles == 0
            ? 0.0
            : 100.0 * static_cast<double>(seqLen * cyclesPerToken) /
                  rawCycles;

    DPRINTF(Accel,
            "LpuAccel: seq_len=%d model_dim=%d -> %d cycles/token, "
            "setup=%d pipeline_fill=%d, %d raw cycles, %d after "
            "util_factor=%.2f (%.1f%% of raw cycles were useful "
            "streaming work)\n",
            seqLen, modelDim, cyclesPerToken, setupCycles, pipelineDepth,
            rawCycles, cycles, utilFactor, amortizedPct);
    return Cycles(cycles);
}

} // namespace gem5
