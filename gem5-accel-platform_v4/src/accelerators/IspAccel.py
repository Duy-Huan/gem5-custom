from m5.params import *
from m5.objects.AccelBase import AccelBase


class IspAccel(AccelBase):
    """Fixed-depth streaming ISP (image signal processing) pipeline model."""

    type = "IspAccel"
    cxx_header = "accelerators/isp_accel.hh"
    cxx_class = "gem5::IspAccel"

    pixels_per_cycle = Param.Unsigned(
        4, "Pixels processed per cycle once the pipeline is full"
    )
    stage_overhead_cycles = Param.Unsigned(
        8,
        "Handshake/buffering overhead (cycles) charged per active "
        "pipeline stage (PARAM1 = num_stages), replacing the flat "
        "'+1 cycle per stage' of the v1 model. Set higher for designs "
        "with more elaborate inter-stage buffering.",
    )
    util_factor = Param.Float(
        0.9,
        "Overall derating factor in (0,1] applied on top of the "
        "analytical cycle count, same rationale as NpuAccel.util_factor.",
    )
