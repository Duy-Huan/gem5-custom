# NPU SimObject - Test & Integration Guide

## Table of Contents
1. [Design Specification](#1-design-specification)
2. [Environment Setup](#2-environment-setup)
3. [Build Instructions](#3-build-instructions)
4. [Test Execution](#4-test-execution)
5. [Troubleshooting](#5-troubleshooting)

---

## 1. Design Specification

### 1.1 Architecture Overview

The NPU SimObject is a cycle-accurate AI accelerator model integrated into gem5's simulation framework. It communicates with the CPU via MMIO (Memory-Mapped I/O) and with system memory via DMA (Direct Memory Access).

### 1.2 Key Components

#### CSR Callback Registry
- **Purpose**: Decoupled register management using std::function callbacks
- **Benefits**: Extensible, testable, supports special write behaviors (write-1-to-clear, write-1-to-set)
- **Implementation**: `CsrCallbackRegistry` class in `npu_csr_registry.hh/cc`

#### DMA Engine
- **Chunking**: Splits large transfers into bus-width sized packets
- **Backpressure**: Handles blocked ports via `recvReqRetry()`
- **States**: IDLE, READ_PENDING, WRITE_PENDING

#### Compute Engine
- **Latency Model**: Cycle-accurate based on PE lanes and operation type
- **Operations**: CONV2D, DEPTHWISE, MATMUL, ADD, RELU, SILU, MAXPOOL, AVGPOOL
- **Instruction Format**: 64-byte fixed size, 17 fields

### 1.3 State Machine

```
IDLE --(START)--> FETCH_INSTR --(DMA_DONE)--> EXECUTING
  ^                                              |
  |                                              v
  +--(DONE clear)--- DONE <--(OP_END)--+---- DMA_WAIT
                                       |        ^
                                       +--(DMA_DONE)
```

---

## 2. Environment Setup

### 2.1 System Requirements

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| OS | Ubuntu 20.04 LTS | Ubuntu 22.04 LTS |
| CPU | x86_64, 4 cores | x86_64, 8+ cores |
| RAM | 16 GB | 32 GB |
| Disk | 50 GB free | 100 GB SSD |

### 2.2 Dependencies

```bash
# System packages
sudo apt-get install -y build-essential git m4 scons zlib1g-dev \
    libprotobuf-dev protobuf-compiler libprotoc-dev \
    libgoogle-perftools-dev python3-dev python3-pip \
    libboost-all-dev pkg-config libhdf5-serial-dev libpng-dev

# Python packages
pip3 install --user numpy pandas matplotlib pydot protobuf pyyaml

# Google Test (for C++ unit tests)
sudo apt-get install -y libgtest-dev cmake
cd /usr/src/gtest && sudo cmake . && sudo make
sudo cp lib/*.a /usr/lib/
```

### 2.3 gem5 Installation

```bash
export GEM5_SRC="$HOME/gem5"
git clone https://github.com/gem5/gem5.git "$GEM5_SRC"
cd "$GEM5_SRC" && git checkout v23.0.0.0
```

---

## 3. Build Instructions

### 3.1 Integrate NPU Source

```bash
# From project root
cp -r src/npu/* $GEM5_SRC/src/npu/

# Update src/SConscript to include:
# SimObject('npu/NpuSimObject.py')
```

### 3.2 Build gem5

```bash
cd $GEM5_SRC
scons build/ARM/gem5.opt -j$(nproc)
```

### 3.3 Build C++ Unit Tests

```bash
./scripts/build_unit_tests.sh
```

This compiles:
- `test_csr_registry`
- `test_state_machine`
- `test_dma_chunking`

---

## 4. Test Execution

### 4.1 Run All Tests

```bash
./scripts/run_all_tests.sh
```

Output:
```
==========================================
NPU SimObject - Test Suite
==========================================

Running: test_csr_registry
  [PASS] test_csr_registry

Running: test_state_machine
  [PASS] test_state_machine

Running: test_dma_chunking
  [PASS] test_dma_chunking

Running: test_mmio_atomic.py
  [PASS] test_mmio_atomic.py

Running: test_backdoor_load.py
  [PASS] test_backdoor_load.py

Running: test_port_interface.py
  [PASS] test_port_interface.py

==========================================
SUMMARY: Total=6 Passed=6 Failed=0
==========================================
ALL TESTS PASSED!
```

### 4.2 Run Individual Tests

**C++ Unit Tests:**
```bash
# CSR Registry tests
./tests/npu/bin/test_csr_registry

# With filter
./tests/npu/bin/test_csr_registry --gtest_filter="*WriteClear*"

# Verbose output
./tests/npu/bin/test_csr_registry --gtest_also_run_disabled_tests -V
```

**gem5 Integration Tests:**
```bash
cd tests/npu/integration
python3 test_mmio_atomic.py
python3 test_backdoor_load.py
python3 test_port_interface.py
```

**With Debug Flags:**
```bash
$GEM5_SRC/build/ARM/gem5.opt --debug-flags=NpuSimObject \
    configs/example/npu_minimal.py
```

### 4.3 Test Categories

| Category | Command | Time |
|----------|---------|------|
| Fast (C++ only) | `./run_all_tests.sh --fast` | ~30s |
| Unit tests | `./run_all_tests.sh --unit` | ~1min |
| Integration | `./run_all_tests.sh --integration` | ~5min |
| Full suite | `./run_all_tests.sh` | ~10min |

---

## 5. Troubleshooting

### Common Issues

| Issue | Solution |
|-------|----------|
| `SimObject not found` | Check `src/SConscript` includes `SimObject('npu/NpuSimObject.py')` |
| `undefined reference` | Verify `SConscript` has `Source('npu_sim_object.cc')` |
| `Port not connected` | Connect both `cpu_port` and `mem_port` in config |
| `DMA hangs` | Check `mem_port` connects to `membus.mem_side_ports` |
| `Backdoor fails` | Use absolute path for `weight_file_path` |
| `Stats missing` | Ensure `NpuStats` parent is set to `this` |

### Debug Commands

```bash
# Verify NPU in binary
strings build/ARM/gem5.opt | grep -i "NpuSimObject"

# Check compiled objects
find build/ARM -name "*npu*" -type f

# Verify Python params
python3 -c "from m5.objects import NpuSimObject; print(NpuSimObject._params.keys())"
```

### Debug Flags

| Flag | Description |
|------|-------------|
| `NpuSimObject` | NPU-specific debug output |
| `DRAM` | Memory controller debug |
| `MemoryAccess` | All memory accesses |

```bash
./build/ARM/gem5.opt --debug-flags=NpuSimObject,DRAM <config.py>
```
