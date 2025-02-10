#!/bin/bash
set -xe

mkdir -p build
cd build
BUILD_DIR=$(pwd)

# RDMA Core
sudo apt-get install -y rdma-core

# GDRCopy
wget -O gdrcopy-2.4.4.tar.gz https://github.com/NVIDIA/gdrcopy/archive/refs/tags/v2.4.4.tar.gz
tar xf gdrcopy-2.4.4.tar.gz
cd gdrcopy-2.4.4/
make prefix="$BUILD_DIR/gdrcopy" \
    CUDA=/usr/local/cuda \
    -j$(nproc --all) all install
cd ..
export LD_LIBRARY_PATH="$BUILD_DIR/gdrcopy/lib:$LD_LIBRARY_PATH"

# libfabric
wget https://github.com/ofiwg/libfabric/releases/download/v2.0.0/libfabric-2.0.0.tar.bz2
tar xf libfabric-2.0.0.tar.bz2
cd libfabric-2.0.0
./configure --prefix="$BUILD_DIR/libfabric" \
    --with-cuda=/usr/local/cuda \
    --with-gdrcopy="$BUILD_DIR/gdrcopy"
make -j$(nproc --all)
make install
cd ..
export LD_LIBRARY_PATH="$BUILD_DIR/libfabric/lib:$LD_LIBRARY_PATH"

# fabtests
wget https://github.com/ofiwg/libfabric/releases/download/v2.0.0/fabtests-2.0.0.tar.bz2
tar xf fabtests-2.0.0.tar.bz2
cd fabtests-2.0.0
./configure --prefix="$BUILD_DIR/fabtests" \
    --with-cuda=/usr/local/cuda \
    --with-libfabric="$BUILD_DIR/libfabric"
make -j$(nproc --all)
make install
cd ..
