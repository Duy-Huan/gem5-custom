#ifndef __ACCELERATORS_ISP_ACCEL_HH__
#define __ACCELERATORS_ISP_ACCEL_HH__

#include "accelerators/accel_base.hh"
#include "params/IspAccel.hh"

namespace gem5
{

/**
 * ISP: an image-signal-processing pipeline (e.g. demosaic -> denoise ->
 * color-correct -> tone-map -> ...), modeled as a fixed-depth hardware
 * pipeline that streams pixels through `num_stages` stages, processing
 * `pixels_per_cycle` pixels per cycle once full.
 *
 * Register usage:
 *   PARAM0 = (width << 16) | height   (frame dimensions, 16 bits each)
 *   PARAM1 = num_stages               (active pipeline stages for this call)
 *   LEN_BYTES = raw frame size in bytes DMA'd in/out.
 *
 * Timing model (v2):
 *     total_pixels = width * height
 *     rawCycles = ceil(total_pixels / pixels_per_cycle)
 *                 + num_stages * stage_overhead_cycles
 *     cycles = ceil(rawCycles / util_factor)
 *
 * v1 charged a flat +1 cycle of fill latency per stage, which understates
 * real pipelines where each stage boundary also costs a few cycles of
 * handshake/buffering (not just one pipeline register). stage_overhead_
 * cycles makes that per-stage cost configurable instead of hardcoded to
 * 1, so num_stages now has a real, tunable cost - e.g. a 10-stage HDR
 * pipeline should look visibly more expensive per-frame than a 3-stage
 * one even at the same resolution. util_factor is the same general
 * derating knob as in NpuAccel - see its doc-comment for the rationale.
 */
class IspAccel : public AccelBase
{
  public:
    using Params = IspAccelParams;
    IspAccel(const Params &p);

  protected:
    Cycles computeLatency(uint64_t lenBytes, uint64_t param0,
                           uint64_t param1) const override;
    const char *accelName() const override { return "IspAccel"; }

  private:
    const unsigned pixelsPerCycle;
    const unsigned stageOverheadCycles;
    const double utilFactor;
};

} // namespace gem5

#endif // __ACCELERATORS_ISP_ACCEL_HH__
