#!/usr/bin/env python3
"""TC_12.1, TC_12.2: Port interface tests"""
import m5
from m5.objects import *

def test_ports():
    print("="*60)
    print("TEST: TC_12.1_TC_12.2_PortInterface")
    print("="*60)

    system = System()
    system.clk_domain = SrcClockDomain(clock='1GHz')
    system.membus = SystemXBar()

    system.npu = NpuSimObject(
        cpu_port=system.membus.cpu_side_ports,
        mem_port=system.membus.mem_side_ports,
        num_pe_lanes=512, sram_size_kb=1024, bus_width_bytes=32,
        pio_addr=0x40000000
    )

    cpu_p = system.npu.getPort("cpu_port")
    mem_p = system.npu.getPort("mem_port")
    assert cpu_p is not None, "cpu_port is None"
    assert mem_p is not None, "mem_port is None"
    print("  [PASS] getPort('cpu_port') valid")
    print("  [PASS] getPort('mem_port') valid")

    ranges = system.npu.getAddrRanges()
    assert len(ranges) == 1
    for r in ranges:
        assert r.start == 0x40000000
        assert r.end == 0x40000FFF
    print("  [PASS] Address range [0x40000000, 0x40000FFF]")

    print("TC_12.1_TC_12.2: PASSED")
    return True

if __name__ == '__main__':
    test_ports()
