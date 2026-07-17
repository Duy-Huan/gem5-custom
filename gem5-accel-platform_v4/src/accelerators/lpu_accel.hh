#ifndef __ACCELERATORS_LPU_ACCEL_HH__
#define __ACCELERATORS_LPU_ACCEL_HH__

#include "accelerators/accel_base.hh"
#include "params/LpuAccel.hh"

namespace gem5
{

/**
 * LPU: a streaming, dataflow-style sequence processor (e.g. an
 * autoregressive token pipeline, in the spirit of dataflow LPU designs
 * such as Groq's). Unlike the NPU's compute-bound systolic model, the LPU
 * is modeled as throughput-bound: it processes a fixed number of "lanes"
 * (vector width) worth of a token's hidden dimension per cycle, streaming
 * tokens one after another through a fixed-depth pipeline.
 *
 * Register usage:
 *   PARAM0 = seq_len   (number of tokens in this batch/call)
 *   PARAM1 = model_dim (hidden dimension per token)
 *   LEN_BYTES = bytes of activations DMA'd in/out for this call.
 *
 * Timing model (v2):
 *     cycles_per_token = ceil(model_dim / lanes_per_cycle)
 *     rawCycles = setup_cycles + seq_len * cycles_per_token + pipeline_depth
 *     cycles = ceil(rawCycles / util_factor)
 *
 * setup_cycles is a *per-call* fixed cost (independent of seq_len) that
 * models loading model weights / KV-cache pointers / context state before
 * streaming can begin - this is the "padding waste" analog for the LPU:
 * calling the device with many small seq_len batches pays this cost
 * repeatedly, so amortizing it (bigger batches) is explicitly rewarded
 * by this model, just like the NPU rewards output tiles that fully use
 * the array. pipeline_depth (fill latency, independent of seq_len) is
 * unchanged from v1. util_factor is the same general derating knob as
 * in NpuAccel - see its doc-comment for the rationale.
 */
class LpuAccel : public AccelBase
{
  public:
    using Params = LpuAccelParams;
    LpuAccel(const Params &p);

  protected:
    Cycles computeLatency(uint64_t lenBytes, uint64_t param0,
                           uint64_t param1) const override;
    const char *accelName() const override { return "LpuAccel"; }

  private:
    const unsigned lanesPerCycle;
    const unsigned pipelineDepth;
    const unsigned setupCycles;
    const double utilFactor;
};

} // namespace gem5

#endif // __ACCELERATORS_LPU_ACCEL_HH__
