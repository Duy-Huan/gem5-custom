from m5.params import *
from m5.objects.AccelBase import AccelBase


class NpuAccel(AccelBase):
    """Systolic-array NPU (matrix-multiply) accelerator model."""

    type = "NpuAccel"
    cxx_header = "accelerators/npu_accel.hh"
    cxx_class = "gem5::NpuAccel"

    pe_rows = Param.Unsigned(16, "Systolic array rows (processing elements)")
    pe_cols = Param.Unsigned(16, "Systolic array cols (processing elements)")

    weight_load_cycles = Param.Unsigned(
        64,
        "Cycles to load one tile's weights into the PE array before "
        "streaming activations through it (weight-stationary reload "
        "overhead paid once per output tile). Set to 0 to model a design "
        "that fully hides weight loading behind compute.",
    )
    depthwise_call_overhead_cycles = Param.Unsigned(
        32,
        "Extra fixed cycles charged on top of the tile cost for "
        "DEPTHWISE ops (PARAM1 bit0=1), modeling per-channel "
        "descriptor/control overhead. A depthwise conv layer is issued "
        "as one call per input channel, so this cost is paid Cin times "
        "for one logical layer - set to 0 to model a design with a "
        "dedicated, overhead-free depthwise datapath instead of routing "
        "depthwise convs through the dense array.",
    )
    util_factor = Param.Float(
        0.85,
        "Overall derating factor in (0,1] applied on top of the tiled "
        "analytical cycle count, capturing control-path bubbles, "
        "imperfect double buffering, and other overheads not modeled "
        "explicitly. 1.0 = trust the analytical formula exactly. "
        "Calibrate against real RTL/silicon measurements.",
    )
