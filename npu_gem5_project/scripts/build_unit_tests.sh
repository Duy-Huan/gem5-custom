#!/bin/bash
set -e
GEM5_SRC="${GEM5_SRC:-$HOME/gem5}"
TEST_DIR="$(dirname $0)/../tests/npu"
OUT_DIR="$TEST_DIR/bin"

CXXFLAGS="-std=c++17 -I$GEM5_SRC/src -I$GEM5_SRC/build/ARM -g -O0 -fPIC"
LDFLAGS="-lgtest -lgtest_main -pthread"

mkdir -p "$OUT_DIR"

echo "Building unit tests..."
g++ $CXXFLAGS "$TEST_DIR/unit/test_csr_registry.cc" \
    "$GEM5_SRC/src/npu/npu_csr_registry.cc" $LDFLAGS -o "$OUT_DIR/test_csr_registry"

echo "Building behavior tests..."
g++ $CXXFLAGS "$TEST_DIR/behavior/test_state_machine.cc" $LDFLAGS -o "$OUT_DIR/test_state_machine"
g++ $CXXFLAGS "$TEST_DIR/behavior/test_dma_chunking.cc" $LDFLAGS -o "$OUT_DIR/test_dma_chunking"

echo "Done. Binaries in $OUT_DIR"
ls -la "$OUT_DIR"
