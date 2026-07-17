#ifndef __ACCELERATORS_NPU_ACCEL_HH__
#define __ACCELERATORS_NPU_ACCEL_HH__

#include "accelerators/accel_base.hh"
#include "base/statistics.hh"
#include "params/NpuAccel.hh"

namespace gem5
{

/**
 * NPU: a systolic-array matrix-multiply engine (M x K x N GEMM), modeled
 * after common weight-stationary NPU designs. v3 adds real CNN-layer
 * awareness: PARAM1 now carries a layer-type flag so *depthwise*
 * convolutions (the operator MobileNet-style networks are built from) are
 * timed differently from *dense* GEMM-shaped ops (regular conv2d,
 * pointwise/1x1 conv, fully-connected) - see the "dense vs depthwise"
 * section below. This is what lets the model show, e.g., why a
 * MobileNetV2-style network can look cheap in raw MAC/FLOP count but poor
 * in NPU utilization: depthwise layers just don't map onto a dense
 * systolic array the way a normal conv/GEMM does.
 *
 * Register usage for this accelerator:
 *   PARAM0 = (M << 32) | (K << 16) | N     (each dimension a 16-bit count)
 *   PARAM1 = bit0: layer_type (0 = DENSE, 1 = DEPTHWISE)
 *            all other bits reserved (e.g. future dtype/precision flags)
 *   LEN_BYTES = total bytes of the *input* activation+weight blob that
 *               gets DMA'd in, and (for this simple model) the same size
 *               buffer DMA'd out as the result.
 *
 * --- Mapping a real CNN layer onto (M, K, N, layer_type) ---
 * Standard conv2d / pointwise (1x1) conv / fully-connected are all dense
 * GEMMs via the usual im2col construction: for an OhxOwxCout output
 * computed from a KhxKwxCin kernel,
 *     M = Oh * Ow                      (output spatial positions)
 *     K = Kh * Kw * Cin                (reduction: patch size x in-chans)
 *     N = Cout                         (output channels)
 *     layer_type = DENSE
 * A *depthwise* conv (groups == Cin, one filter per input channel) is
 * fundamentally NOT a single dense GEMM: output channel c only depends
 * on input channel c, so there is no shared reduction across channels.
 * The realistic way to run it on a dense systolic array is as Cin
 * separate small ops, one per channel:
 *     M = Oh * Ow, K = Kh * Kw, N = 1, layer_type = DEPTHWISE   (x Cin)
 * With N = 1, a tile only ever uses 1 of pe_cols columns - see
 * computeLatency() for how that (correctly) shows up as poor
 * utilization. workloads/tools/model_to_npu_calls.py performs this
 * expansion automatically from a JSON model description.
 *
 * --- Timing model (v2 tiling, unchanged) ---
 * A pe_rows x pe_cols weight-stationary systolic array computes the M x N
 * output matrix one pe_rows x pe_cols *tile* at a time. Each tile requires
 * the weights for that tile to be loaded, then K activation values
 * streamed through, which takes K + (pe_rows + pe_cols - 1) cycles
 * (streaming + pipeline fill/drain). The array is only ever partially
 * utilized in the final row/col of tiles when M or N is not an exact
 * multiple of pe_rows/pe_cols - this "padding waste" is explicit in the
 * model because tiles are counted with ceil(), not by dividing total
 * MACs by array size:
 *
 *   numTilesM = ceil(M / pe_rows)
 *   numTilesN = ceil(N / pe_cols)
 *   cyclesPerTile = weight_load_cycles + K + (pe_rows + pe_cols - 1)
 *   rawCycles = numTilesM * numTilesN * cyclesPerTile
 *
 * --- v3 addition: depthwise call overhead ---
 * Real hardware also pays a small control-plane cost (descriptor
 * generation, DMA setup) *per accelerator call*, which for DENSE layers
 * is negligible relative to the useful work in one call, but for
 * DEPTHWISE layers - issued once per channel, i.e. Cin times for one
 * logical layer - adds up and is worth modeling explicitly:
 *
 *   rawCycles += (layer_type == DEPTHWISE) ? depthwise_call_overhead_cycles
 *                                            : 0
 *   cycles = ceil(rawCycles / util_factor)
 *
 * `util_factor` (0 < util_factor <= 1) is still the general derating
 * knob for everything not modeled explicitly - see NpuAccel.py.
 */
class NpuAccel : public AccelBase
{
  public:
    using Params = NpuAccelParams;
    NpuAccel(const Params &p);

  protected:
    Cycles computeLatency(uint64_t lenBytes, uint64_t param0,
                           uint64_t param1) const override;
    const char *accelName() const override { return "NpuAccel"; }

  private:
    const unsigned peRows;
    const unsigned peCols;
    const unsigned weightLoadCycles;
    const unsigned depthwiseCallOverheadCycles;
    const double utilFactor;

    struct NpuLayerStats : public statistics::Group
    {
        NpuLayerStats(NpuAccel &a);
        statistics::Scalar denseOps;
        statistics::Scalar depthwiseOps;
        statistics::Scalar macsComputed; // theoretical, M*K*N summed
    };
    // mutable: computeLatency() is logically const (pure function of its
    // arguments as far as the FSM/timing model is concerned) but still
    // needs to update these bookkeeping counters as a side effect.
    mutable NpuLayerStats layerStats;
};

} // namespace gem5

#endif // __ACCELERATORS_NPU_ACCEL_HH__
