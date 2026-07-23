#!/bin/bash
set -e
echo "=========================================="
echo "NPU SimObject - Environment Setup"
echo "=========================================="

sudo apt-get update
sudo apt-get install -y build-essential git m4 scons zlib1g-dev \
    libprotobuf-dev protobuf-compiler libprotoc-dev \
    libgoogle-perftools-dev python3-dev python3-pip \
    libboost-all-dev pkg-config libhdf5-serial-dev libpng-dev

pip3 install --user numpy pandas matplotlib pydot protobuf pyyaml

sudo apt-get install -y libgtest-dev cmake
cd /usr/src/gtest && sudo cmake . && sudo make && sudo cp lib/*.a /usr/lib/

GEM5_DIR="$HOME/gem5"
if [ ! -d "$GEM5_DIR" ]; then
    git clone https://github.com/gem5/gem5.git "$GEM5_DIR"
    cd "$GEM5_DIR" && git checkout v23.0.0.0
fi

cat >> ~/.bashrc << 'EOF'
export GEM5_SRC="$HOME/gem5"
export GEM5_BUILD="$HOME/gem5/build"
export PYTHONPATH="$GEM5_SRC/src/python:$PYTHONPATH"
EOF

echo "Setup complete. Run: source ~/.bashrc"
