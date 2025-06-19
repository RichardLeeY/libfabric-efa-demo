#!/bin/bash
set -xe

mkdir -p build
cd build
BUILD_DIR=$(pwd)

# RDMA Core
sudo dnf install -y rdma-core


# libfabric
wget https://github.com/ofiwg/libfabric/releases/download/v2.0.0/libfabric-2.0.0.tar.bz2
tar xf libfabric-2.0.0.tar.bz2
cd libfabric-2.0.0
./configure --prefix="$BUILD_DIR/libfabric" \
        --enable-debug
make -j$(nproc --all)
make install
cd ..
export LD_LIBRARY_PATH="$BUILD_DIR/libfabric/lib:$LD_LIBRARY_PATH"

# fabtests
wget https://github.com/ofiwg/libfabric/releases/download/v2.0.0/fabtests-2.0.0.tar.bz2
tar xf fabtests-2.0.0.tar.bz2
cd fabtests-2.0.0
./configure --prefix="$BUILD_DIR/fabtests" \
    --with-libfabric="$BUILD_DIR/libfabric"
make -j$(nproc --all)
make install
cd ..