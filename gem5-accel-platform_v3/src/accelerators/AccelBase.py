from m5.params import *
from m5.proxy import *
from m5.objects.Device import DmaDevice


class AccelBase(DmaDevice):
    """
    Common base for all company accelerator models (NPU/LPU/ISP).
    Do not instantiate directly - use NpuAccel / LpuAccel / IspAccel.
    """

    type = "AccelBase"
    abstract = True
    cxx_header = "accelerators/accel_base.hh"
    cxx_class = "gem5::AccelBase"

    pio_addr = Param.Addr("MMIO base address of the register file")
    pio_latency = Param.Latency(
        "1ns", "Fixed latency for register-file MMIO accesses"
    )
    accel_id = Param.Int(
        "Unique integer id for this accelerator instance. Used by the "
        "SE-mode gem5_accel syscall shim to address this specific device "
        "(0=first NPU, 1=first LPU, ... assign consistently with the "
        "config script and the benchmark source)."
    )
