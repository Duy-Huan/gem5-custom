#!/bin/bash
set -e
GEM5_SRC="${GEM5_SRC:-$HOME/gem5}"
NPU_SRC="$(dirname $0)/../src/npu"

echo "Integrating NPU into gem5..."

mkdir -p "$GEM5_SRC/src/npu"
cp "$NPU_SRC"/* "$GEM5_SRC/src/npu/"

MAIN_SC="$GEM5_SRC/src/SConscript"
if ! grep -q "npu/NpuSimObject" "$MAIN_SC"; then
    echo "Adding NPU to SConscript..."
    # Add after existing SimObject lines
    sed -i '/^SimObject/a SimObject('''npu/NpuSimObject.py''')' "$MAIN_SC"
fi

echo "Integration complete. Build with:"
echo "  cd $GEM5_SRC && scons build/ARM/gem5.opt -j\$(nproc)"
