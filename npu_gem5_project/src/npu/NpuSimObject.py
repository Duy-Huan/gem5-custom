from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject

class NpuSimObject(SimObject):
    type = 'NpuSimObject'
    cxx_header = "npu/npu_sim_object.hh"
    cxx_class = 'gem5::NpuSimObject'

    cpu_port = ResponsePort("CPU-side MMIO/CSR port")
    mem_port = RequestPort("Memory-side DMA port")

    num_pe_lanes = Param.Int(512, "Number of MAC units: 256, 512, 1024, 2048")
    sram_size_kb = Param.Int(1024, "Internal SRAM in KB: 512-4096")
    bus_width_bytes = Param.Int(32, "AXI bus width: 16, 32, 64")
    clock_freq = Param.Clock("1GHz", "NPU clock")
    weight_file_path = Param.String("", "Host .bin file for backdoor load")
    pio_addr = Param.Addr(0x40000000, "MMIO base")
    pio_size = Param.Addr(0x1000, "MMIO size (4KB)")
