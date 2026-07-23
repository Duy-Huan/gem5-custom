#!/bin/bash
set -e
GEM5_SRC="${GEM5_SRC:-$HOME/gem5}"
TEST_DIR="$(dirname $0)/../tests/npu"
RESULTS="$TEST_DIR/results/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS"

TOTAL=0; PASSED=0; FAILED=0

echo "=========================================="
echo "NPU SimObject - Test Suite"
echo "=========================================="

# C++ Unit Tests
for t in test_csr_registry test_state_machine test_dma_chunking; do
    TOTAL=$((TOTAL+1))
    echo ""; echo "Running: $t"
    if [ -f "$TEST_DIR/bin/$t" ]; then
        if "$TEST_DIR/bin/$t" > "$RESULTS/${t}.log" 2>&1; then
            echo "  [PASS] $t"; PASSED=$((PASSED+1))
        else
            echo "  [FAIL] $t"; FAILED=$((FAILED+1))
        fi
    else
        echo "  [SKIP] $t (not built)"
    fi
done

# gem5 Integration Tests
for t in test_mmio_atomic.py test_backdoor_load.py test_port_interface.py; do
    TOTAL=$((TOTAL+1))
    echo ""; echo "Running: $t"
    if [ -f "$TEST_DIR/integration/$t" ]; then
        cd "$TEST_DIR/integration"
        if python3 "$t" > "$RESULTS/${t%.py}.log" 2>&1; then
            echo "  [PASS] $t"; PASSED=$((PASSED+1))
        else
            echo "  [FAIL] $t"; FAILED=$((FAILED+1))
        fi
    else
        echo "  [SKIP] $t (not found)"
    fi
done

echo ""
echo "=========================================="
echo "SUMMARY: Total=$TOTAL Passed=$PASSED Failed=$FAILED"
echo "Results: $RESULTS"
echo "=========================================="

[ $FAILED -eq 0 ] && echo "ALL TESTS PASSED!" && exit 0 || exit 1
