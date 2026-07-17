from m5.params import *
from m5.objects.AccelBase import AccelBase


class LpuAccel(AccelBase):
    """Streaming dataflow LPU (sequence/token processor) accelerator model."""

    type = "LpuAccel"
    cxx_header = "accelerators/lpu_accel.hh"
    cxx_class = "gem5::LpuAccel"

    lanes_per_cycle = Param.Unsigned(
        256, "Hidden-dimension elements processed per cycle (vector width)"
    )
    pipeline_depth = Param.Unsigned(
        32, "Fixed dataflow pipeline fill/drain latency, in cycles"
    )
    setup_cycles = Param.Unsigned(
        64,
        "Fixed per-call overhead (cycles) to load model weights / "
        "KV-cache pointers / context state before token streaming can "
        "begin. Independent of seq_len, so small batches pay it "
        "relatively more - this rewards larger batch sizes in the model, "
        "matching real dataflow LPU behavior.",
    )
    util_factor = Param.Float(
        0.9,
        "Overall derating factor in (0,1] applied on top of the "
        "analytical cycle count, same rationale as NpuAccel.util_factor.",
    )
