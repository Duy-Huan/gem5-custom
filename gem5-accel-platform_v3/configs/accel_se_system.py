"""
accel_se_system.py - builds a Syscall-Emulation (SE) mode RISC-V gem5
system with three memory-mapped accelerators (NPU, LPU, ISP) attached to
the system crossbar, and runs a given RISC-V binary on it.

Drop this file into your gem5 checkout's configs/ directory (or run it
with an absolute path) and launch with:

    build/RISCV/gem5.opt configs/accel_se_system.py \
        --binary workloads/npu_matmul_bench

Prerequisites:
  - You've built gem5 for RISCV: `scons build/RISCV/gem5.opt -j<N>`
    from your gem5 checkout root, with src/accelerators/ (this package)
    and the gem5_accel syscall patch (see docs/INTEGRATION.md) applied.
  - The binary was cross-compiled per workloads/Makefile.

Notable stats to look at afterwards in m5out/stats.txt for the goals
in your original questions:
  - Bus/interconnect throughput & contention:
      system.membus.trans_dist / system.membus.pkt_count_system[...]
      system.membus.snoop_data etc. (values under `system.membus.*`)
  - Memory controller bandwidth:
      system.mem_ctrl.dram.bw_read/write::total (or bw_total, depending
      on gem5 version), system.mem_ctrl.dram.busUtil
  - Accelerator utilization / throughput (this package's stats):
      system.accel_npu.opsCompleted, .bytesTransferred, .busyCycles
      system.accel_lpu.*  system.accel_isp.*
  - Overall run: system.cpu.numCycles, system.cpu.ipc (if applicable)
"""

import argparse

import m5
from m5.objects import (
    AddrRange,
    IspAccel,
    LpuAccel,
    MemCtrl,
    DDR3_1600_8x8,
    NpuAccel,
    Process,
    RiscvEmuLinux,
    Root,
    System,
    SystemXBar,
    TimingSimpleCPU,
    SrcClockDomain,
    VoltageDomain,
)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--binary", required=True, help="Path to a "
                     "statically-linked riscv64 binary (see workloads/)")
parser.add_argument("--options", default="", help="Command-line "
                     "arguments to pass to the binary")
parser.add_argument("--cpu-clock", default="2GHz")
parser.add_argument("--mem-size", default="256MB")
args = parser.parse_args()

system = System()

system.clk_domain = SrcClockDomain()
system.clk_domain.clock = args.cpu_clock
system.clk_domain.voltage_domain = VoltageDomain()

system.mem_mode = "timing"
system.mem_ranges = [AddrRange(args.mem_size)]

# --- CPU ---------------------------------------------------------------
# TimingSimpleCPU keeps the CPU-side timing simple so the numbers you
# measure are dominated by the accelerator/bus/memory models below.
# Swap in O3CPU (from m5.objects import O3CPU) for a detailed
# out-of-order pipeline if you also want to study CPU-side effects, e.g.
# the driver loop's polling overhead.
system.cpu = TimingSimpleCPU()

# --- Interconnect --------------------------------------------------------
system.membus = SystemXBar()
system.system_port = system.membus.cpu_side_ports

system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports

# --- Main memory ---------------------------------------------------------
system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

# --- Accelerators ----------------------------------------------------------
# MMIO windows placed just above the end of DRAM. Each window is
# AccelBase.RegWindowSize (0x40) bytes; we space them out generously to
# leave headroom if you widen the register file later.
MMIO_BASE = system.mem_ranges[0].end + 1
MMIO_STRIDE = 0x1000

system.accel_npu = NpuAccel(
    pio_addr=MMIO_BASE + 0 * MMIO_STRIDE, accel_id=0, pe_rows=16, pe_cols=16
)
system.accel_lpu = LpuAccel(
    pio_addr=MMIO_BASE + 1 * MMIO_STRIDE, accel_id=1,
    lanes_per_cycle=256, pipeline_depth=32,
)
system.accel_isp = IspAccel(
    pio_addr=MMIO_BASE + 2 * MMIO_STRIDE, accel_id=2, pixels_per_cycle=4
)

for dev in (system.accel_npu, system.accel_lpu, system.accel_isp):
    # PioDevice's response port receives MMIO requests from the bus.
    dev.pio = system.membus.mem_side_ports
    # DmaDevice's request port issues DMA reads/writes onto the bus.
    dev.dma = system.membus.cpu_side_ports
    dev.clk_domain = system.clk_domain

# --- Workload (SE mode) ---------------------------------------------------
system.workload = RiscvEmuLinux()
process = Process()
process.cmd = [args.binary] + args.options.split()
system.cpu.workload = process
system.cpu.createThreads()

root = Root(full_system=False, system=system)
m5.instantiate()

print(f"Running {args.binary} ...")
exit_event = m5.simulate()
print(
    f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}"
)
