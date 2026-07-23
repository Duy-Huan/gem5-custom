# NPU SimObject for gem5 v23+

## Overview

Full-System AI Accelerator (NPU) simulation model for gem5, featuring:
- **Pure C++ SimObject** with native gem5 API (ResponsePort/RequestPort)
- **CSR Callback Registry** for extensible MMIO register management
- **DMA Engine** with chunking, backpressure, and retry support
- **Backdoor Fast-Load** for weights from host filesystem
- **Cycle-accurate Event System** for compute scheduling
- **Comprehensive Test Suite** (C++ unit tests + gem5 integration tests)

## Architecture

```
CPU (ARM64/RISC-V)
    |
    | MMIO Access (Atomic/Timing)
    v
[NPU CSR Port] -- ResponsePort (Slave)
    |
    | Internal
    v
[CSR Registry] --> [Compute Engine] --> [DMA Engine]
    |                                      |
    |                                      | RequestPort (Master)
    v                                      v
SRAM (512KB-4MB)                    System Bus -> DRAM
```

## CSR Register Map

| Offset | Name | Access | Description |
|--------|------|--------|-------------|
| 0x00 | REG_CTRL | R/W | START, RESET, INT_EN |
| 0x04 | REG_STATUS | R/W1C | IDLE, BUSY, DONE, ERROR |
| 0x08-0x0C | REG_WEIGHT_ADDR | R/W | 64-bit weight buffer address |
| 0x10-0x14 | REG_INSTR_ADDR | R/W | 64-bit instruction stream address |
| 0x18-0x1C | REG_FEATURE_SRC | R/W | 64-bit input tensor address |
| 0x20-0x24 | REG_FEATURE_DST | R/W | 64-bit output tensor address |
| 0x28 | REG_INT_CLEAR | WO | Write 1 to clear interrupt |
| 0x2C | REG_VERSION | RO | Hardware version (0x00010000) |
| 0x30 | REG_PE_CONFIG | RO | {PE_LANES, BUS_WIDTH} |
| 0x34 | REG_DMA_STATUS | RO | DMA state |
| 0x38-0x3C | REG_CYCLE_COUNT | RO | 64-bit active cycle counter |

## Quick Start

### 1. Environment Setup

```bash
./scripts/setup_env.sh
source ~/.bashrc
```

### 2. Integrate into gem5

```bash
./scripts/integrate_npu.sh
cd $GEM5_SRC
scons build/ARM/gem5.opt -j$(nproc)
```

### 3. Build Tests

```bash
./scripts/build_unit_tests.sh
```

### 4. Run Tests

```bash
./scripts/run_all_tests.sh
```

### 5. Run Simulation

```bash
$GEM5_SRC/build/ARM/gem5.opt configs/example/npu_minimal.py
```

## Directory Structure

```
npu_gem5_project/
├── src/npu/              # SimObject source code
│   ├── SConscript
│   ├── NpuSimObject.py
│   ├── npu_sim_object.hh
│   ├── npu_sim_object.cc
│   ├── npu_csr_registry.hh
│   └── npu_csr_registry.cc
├── tests/
│   ├── npu/unit/         # C++ unit tests
│   ├── npu/behavior/     # C++ behavior tests
│   └── npu/integration/  # gem5 Python integration tests
├── configs/example/      # Sample gem5 configs
├── scripts/             # Build and setup scripts
└── docs/               # Documentation
```

## Test Coverage

| Category | Tests | Status |
|----------|-------|--------|
| CSR Registry | 7 | Ready |
| State Machine | 6 | Ready |
| DMA Chunking | 13 | Ready |
| Interrupt | 6 | Ready |
| Backpressure | 5 | Ready |
| MMIO Atomic | 4 | Ready |
| Backdoor Load | 3 | Ready |
| Port Interface | 2 | Ready |

## License

BSD 3-Clause (same as gem5)
