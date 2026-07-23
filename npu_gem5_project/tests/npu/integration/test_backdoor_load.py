#!/usr/bin/env python3
"""TC_7.1: Backdoor weight loading"""
import m5, os, tempfile
from m5.objects import *

def test_backdoor():
    print("="*60)
    print("TEST: TC_7.1_Backdoor_Load")
    print("="*60)

    weight_data = bytes([i % 256 for i in range(4096)])
    with tempfile.NamedTemporaryFile(delete=False, suffix='.bin') as f:
        f.write(weight_data)
        wpath = f.name

    system = System()
    system.clk_domain = SrcClockDomain(clock='1GHz')
    system.mem_mode = 'atomic'
    system.mem_ranges = [AddrRange('512MB')]
    system.membus = SystemXBar()

    system.npu = NpuSimObject(
        cpu_port=system.membus.cpu_side_ports,
        mem_port=system.membus.mem_side_ports,
        num_pe_lanes=512, sram_size_kb=64, bus_width_bytes=32,
        pio_addr=0x40000000, weight_file_path=wpath
    )

    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR3_1600_8x8()
    system.mem_ctrl.dram.range = system.mem_ranges[0]
    system.mem_ctrl.port = system.membus.mem_side_ports

    root = Root(full_system=False, system=system)
    m5.instantiate()

    status = system.npu.cpu_port.read(0x40000000 + 0x04, 4)
    assert status & (1 << 4), f"WEIGHT_LOADED not set: {status:#x}"
    print(f"  [PASS] STATUS = {status:#x} (WEIGHT_LOADED)")

    os.unlink(wpath)
    print("TC_7.1: PASSED")
    return True

if __name__ == '__main__':
    test_backdoor()
