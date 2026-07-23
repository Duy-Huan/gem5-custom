#!/usr/bin/env python3
"""TC_3.1, TC_5.1: Atomic MMIO read/write tests"""
import m5
from m5.objects import *

def test_mmio_atomic():
    print("="*60)
    print("TEST: TC_3.1_TC_5.1_MMIO_Atomic_ReadWrite")
    print("="*60)

    system = System()
    system.clk_domain = SrcClockDomain(clock='1GHz')
    system.mem_mode = 'atomic'
    system.mem_ranges = [AddrRange('512MB')]
    system.membus = SystemXBar()

    system.npu = NpuSimObject(
        cpu_port=system.membus.cpu_side_ports,
        mem_port=system.membus.mem_side_ports,
        num_pe_lanes=512, sram_size_kb=1024, bus_width_bytes=32,
        pio_addr=0x40000000
    )

    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR3_1600_8x8()
    system.mem_ctrl.dram.range = system.mem_ranges[0]
    system.mem_ctrl.port = system.membus.mem_side_ports

    root = Root(full_system=False, system=system)
    m5.instantiate()

    proxy = system.npu.cpu_port

    # Test 1: Read VERSION (0x2C)
    version = proxy.read(0x40000000 + 0x2C, 4)
    assert version == 0x00010000, f"VERSION mismatch: {version:#x}"
    print(f"  [PASS] REG_VERSION = {version:#x}")

    # Test 2: Read PE_CONFIG (0x30)
    pe = proxy.read(0x40000000 + 0x30, 4)
    expected = (512 << 16) | 32
    assert pe == expected, f"PE_CONFIG mismatch: {pe:#x}"
    print(f"  [PASS] REG_PE_CONFIG = {pe:#x}")

    # Test 3: Write then read back WEIGHT_ADDR
    proxy.write(0x40000000 + 0x08, 0xDEADBEEF, 4)
    proxy.write(0x40000000 + 0x0C, 0x12345678, 4)
    lo = proxy.read(0x40000000 + 0x08, 4)
    hi = proxy.read(0x40000000 + 0x0C, 4)
    assert lo == 0xDEADBEEF and hi == 0x12345678
    print(f"  [PASS] 64-bit addr write/read")

    # Test 4: STATUS is IDLE
    status = proxy.read(0x40000000 + 0x04, 4)
    assert status & 0x1, f"Not IDLE: {status:#x}"
    print(f"  [PASS] REG_STATUS (IDLE) = {status:#x}")

    print("TC_3.1_TC_5.1: PASSED")
    return True

if __name__ == '__main__':
    test_mmio_atomic()
