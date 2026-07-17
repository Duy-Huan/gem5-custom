from m5.params import *
from m5.objects.AccelBase import AccelBase


class NpuAccelRtl(AccelBase):
    """NPU accelerator whose compute phase is driven by a real Verilated
    RTL core (rtl/npu_mac_core.v) instead of an analytical formula. See
    docs/RTL-INTEGRATION.md for the full design writeup."""

    type = "NpuAccelRtl"
    cxx_header = "accelerators/npu_accel_rtl.hh"
    cxx_class = "gem5::NpuAccelRtl"
