#!/usr/bin/env python3
"""Minimal NPU configuration for gem5 SE mode"""
import m5
from m5.objects import *

system = System()
system.clk_domain = SrcClockDomain(clock='1GHz', voltage_domain=VoltageDomain(voltage='1V'))
system.mem_mode = 'timing'
system.mem_ranges = [AddrRange('1GB')]

system.cpu = TimingSimpleCPU()
system.membus = SystemXBar()

system.npu = NpuSimObject(
    cpu_port=system.membus.cpu_side_ports,
    mem_port=system.membus.mem_side_ports,
    num_pe_lanes=512,
    sram_size_kb=1024,
    bus_width_bytes=32,
    clock_freq='1GHz',
    pio_addr=0x40000000,
    pio_size=0x1000,
    weight_file_path=""
)

system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports

system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

root = Root(full_system=False, system=system)
m5.instantiate()

print("Starting simulation...")
exit_event = m5.simulate()
print(f"Exited at tick {m5.curTick()}: {exit_event.getCause()}")
