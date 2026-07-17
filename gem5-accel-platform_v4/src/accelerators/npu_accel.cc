#include "accelerators/npu_accel.hh"

#include <algorithm>
#include <cmath>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/Accel.hh"

namespace gem5
{

namespace
{
constexpr uint64_t LAYER_TYPE_DENSE = 0;
constexpr uint64_t LAYER_TYPE_DEPTHWISE = 1;
} // anonymous namespace

NpuAccel::NpuAccel(const Params &p) :
    AccelBase(p),
    peRows(p.pe_rows),
    peCols(p.pe_cols),
    weightLoadCycles(p.weight_load_cycles),
    depthwiseCallOverheadCycles(p.depthwise_call_overhead_cycles),
    utilFactor(p.util_factor),
    layerStats(*this)
{
    panic_if(peRows == 0 || peCols == 0,
             "NpuAccel: pe_rows/pe_cols must be > 0");
    panic_if(utilFactor <= 0.0 || utilFactor > 1.0,
             "NpuAccel: util_factor must be in (0, 1], got %f", utilFactor);
}

NpuAccel::NpuLayerStats::NpuLayerStats(NpuAccel &a) :
    statistics::Group(&a),
    ADD_STAT(denseOps, statistics::units::Count::get(),
             "Number of dense (regular conv2d/pointwise/FC) GEMM ops run"),
    ADD_STAT(depthwiseOps, statistics::units::Count::get(),
             "Number of depthwise per-channel micro-ops run"),
    ADD_STAT(macsComputed, statistics::units::Count::get(),
             "Theoretical multiply-accumulate count (M*K*N) summed across "
             "all ops, regardless of achieved array utilization - compare "
             "against busyCycles*pe_rows*pe_cols to see overall efficiency")
{
}

Cycles
NpuAccel::computeLatency(uint64_t lenBytes, uint64_t param0,
                          uint64_t param1) const
{
    const uint64_t M = (param0 >> 32) & 0xFFFF;
    const uint64_t K = (param0 >> 16) & 0xFFFF;
    const uint64_t N = param0 & 0xFFFF;
    const uint64_t layerType = param1 & 0x1;

    panic_if(M == 0 || K == 0 || N == 0,
             "NpuAccel: PARAM0 must encode non-zero M,K,N "
             "((M<<32)|(K<<16)|N)");
    panic_if(layerType == LAYER_TYPE_DEPTHWISE && N != 1,
             "NpuAccel: DEPTHWISE ops must be issued one input channel at "
             "a time (N=1) - got N=%d. See model_to_npu_calls.py, which "
             "performs this expansion automatically from a layer config.",
             N);

    // Tile the M x N output matrix into pe_rows x pe_cols blocks. ceil()
    // here (rather than dividing total MACs by array size) is what makes
    // the "ragged edge" tiles - where M or N isn't an exact multiple of
    // the array dimensions - explicitly cost a full tile's worth of
    // cycles even though part of the array is idle in that tile. For
    // DEPTHWISE ops (N=1) this naturally means only 1 of pe_cols columns
    // is ever useful in the single N-tile - the model doesn't need a
    // special case for that, it just falls out of the tile math.
    const uint64_t numTilesM = (M + peRows - 1) / peRows;
    const uint64_t numTilesN = (N + peCols - 1) / peCols;
    const uint64_t fillDrain = peRows + peCols - 1;
    const uint64_t cyclesPerTile = weightLoadCycles + K + fillDrain;

    uint64_t rawCycles = numTilesM * numTilesN * cyclesPerTile;
    if (layerType == LAYER_TYPE_DEPTHWISE)
        rawCycles += depthwiseCallOverheadCycles;

    const uint64_t cycles = static_cast<uint64_t>(
        std::ceil(static_cast<double>(rawCycles) / utilFactor));

    const uint64_t macs = M * K * N;
    const uint64_t idealMacCycles =
        (macs + (static_cast<uint64_t>(peRows) * peCols) - 1) /
        (static_cast<uint64_t>(peRows) * peCols);
    const double effUtilPct =
        idealMacCycles == 0
            ? 0.0
            : 100.0 * static_cast<double>(idealMacCycles) / cycles;

    if (layerType == LAYER_TYPE_DEPTHWISE)
        layerStats.depthwiseOps++;
    else
        layerStats.denseOps++;
    layerStats.macsComputed += macs;

    DPRINTF(Accel,
            "NpuAccel: %s GEMM %dx%dx%d, tiles=%dx%d, %d cycles/tile "
            "(incl. %d weight-load%s) -> %d raw cycles, %d after "
            "util_factor=%.2f (effective array utilization ~%.1f%%)\n",
            layerType == LAYER_TYPE_DEPTHWISE ? "DEPTHWISE" : "DENSE",
            M, K, N, numTilesM, numTilesN, cyclesPerTile, weightLoadCycles,
            layerType == LAYER_TYPE_DEPTHWISE ? "+call overhead" : "",
            rawCycles, cycles, utilFactor, effUtilPct);
    return Cycles(cycles);
}

} // namespace gem5
