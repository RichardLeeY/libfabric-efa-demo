# Technology Stack

## Core Technologies

- **Language**: C++17
- **Build System**: GNU Make
- **Compiler**: g++ with flags: `-Wall -Werror -std=c++17 -march=native -O2 -g`
- **Primary Library**: libfabric (RDMA communication framework)
- **Provider**: AWS EFA (Elastic Fabric Adapter)
- **Optional**: CUDA (for GPU memory operations)

## Key Libraries

### libfabric
- Version: 2.0.0
- Purpose: Fabric communication abstraction layer
- Provides: RDMA operations, endpoint management, completion queues
- Installation: Built from source in `build/libfabric/`

### RDMA Core
- System package for RDMA device support
- Installed via: `sudo dnf install -y rdma-core`

### fabtests
- Version: 2.0.0
- Purpose: Testing and validation utilities for libfabric
- Installation: Built from source in `build/fabtests/`

## Build Configuration

### Compiler Flags
- `-Wall -Werror`: All warnings enabled, treat warnings as errors
- `-std=c++17`: C++17 standard
- `-march=native`: Optimize for native CPU architecture
- `-O2`: Optimization level 2
- `-g`: Include debug symbols

### Include Paths
- `build/libfabric/include`: libfabric headers
- `/usr/local/cuda/include`: CUDA headers (when applicable)

### Library Paths
- `build/libfabric/lib`: libfabric libraries
- `/usr/local/cuda/lib64`: CUDA libraries (when applicable)

### Linked Libraries
- `-lfabric`: libfabric
- `-lpthread`: POSIX threads

## Common Commands

### Install Dependencies
```bash
bash ./install-deps.sh
```
Installs RDMA core, builds libfabric 2.0.0, and builds fabtests 2.0.0 in the `build/` directory.

### Build All Binaries
```bash
make
```
Builds all demo programs into `build/` directory.

### Clean Build Artifacts
```bash
make clean
```
Removes compiled binaries from `build/` directory.

### Run Specific Demo
```bash
# Server mode
./build/16_write_cpu

# Client mode
./build/16_write_cpu <server_address> [page_size num_pages]
```

### Environment Setup
The Makefile automatically sets:
```bash
export LD_LIBRARY_PATH=$(PWD)/build/libfabric/lib:$LD_LIBRARY_PATH
```

## libfabric Configuration

### Provider Settings
- **Provider**: `efa` (AWS Elastic Fabric Adapter)
- **Fabric**: `efa` or `efa-direct` (for direct hardware access)
- **Endpoint Type**: `FI_EP_RDM` (Reliable Datagram)
- **Capabilities**: `FI_MSG | FI_RMA | FI_LOCAL_COMM | FI_REMOTE_COMM`

### Memory Registration Mode
- `FI_MR_LOCAL`: Local memory registration required
- `FI_MR_VIRT_ADDR`: Virtual address mode
- `FI_MR_ALLOCATED`: Memory must be allocated
- `FI_MR_PROV_KEY`: Provider-managed keys

### Threading Model
- `FI_THREAD_SAFE`: Thread-safe operations

## AWS Infrastructure

### Instance Requirements
- EFA-enabled EC2 instances (e.g., c5n.18xlarge, p4d.24xlarge)
- EFA driver installed
- Security groups configured for EFA traffic

### Helper Scripts
- `launch-instances.sh`: Launch AWS instances
- `assume-role.sh`: AWS role assumption
- `rdma-helper.sh`: RDMA utility functions
