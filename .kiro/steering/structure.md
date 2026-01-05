# Project Structure

## Directory Layout

```
.
├── src/                    # Source code files
├── build/                  # Build artifacts and dependencies
├── docs/                   # Documentation and diagrams
├── data/                   # Test data and system information
├── images/                 # Project images
├── generated-diagrams/     # Auto-generated diagrams
├── .kiro/                  # Kiro AI assistant configuration
├── .q/                     # Q agent configuration
└── .vscode/                # VS Code settings
```

## Source Files (`src/`)

### Demo Programs (Numbered Series)
Progressive examples demonstrating RDMA concepts and optimizations:

- `4_hello.cpp` - Basic hello world example
- `5_reverse.cpp` - Reverse operation demo
- `6_write.cpp` - Basic RDMA write
- `7_queue.cpp` - Queue-based operations
- `8_topo.cpp` - Topology awareness
- `9_multinet.cpp` - Multiple network interfaces
- `10_warmup.cpp` - Connection warmup techniques
- `11_multithread.cpp` - Multithreaded RDMA
- `12_pin.cpp` - CPU pinning optimizations
- `13_shard.cpp` - Data sharding patterns
- `14_batch.cpp` - Batched operations
- `15_lazy.cpp` - Lazy evaluation patterns
- `16_write_cpu.cpp` - RDMA write to CPU memory (main demo)

### Core Examples
- `rdma_send_recv.cpp` - Send/receive pattern implementation
- `rdma_direct.cpp` - Direct RDMA operations
- `rdma_direct_write.cpp` - Direct RDMA write operations

## Build Directory (`build/`)

Generated during installation, contains:
- `libfabric/` - libfabric library installation
- `fabtests/` - libfabric test utilities
- Compiled binaries (e.g., `16_write_cpu`, `rdma_direct_write`)

## Documentation (`docs/`)

- `16_write_cpu.md` - Detailed documentation for the main demo
- `16_write_cpu_cn.md` - Chinese translation
- `16_write_cpu_cn.pdf` - PDF version
- `16_write_cpu_detailed_sequence.png` - Sequence diagram

## Data Directory (`data/`)

System information and test outputs:
- `fi_info.yaml` - Fabric interface information
- `fi_rma_bw.txt` - RMA bandwidth test results
- `ls_sys_bus_pci_devices.txt` - PCI device listing
- `lspci.txt` - PCI device details

## Key Files

### Build & Configuration
- `Makefile` - Build system configuration
- `install-deps.sh` - Dependency installation script

### AWS Utilities
- `launch-instances.sh` - EC2 instance launcher
- `assume-role.sh` - AWS role assumption helper
- `rdma-helper.sh` - RDMA utility functions

### Documentation
- `README.md` - Project overview and build instructions
- `RDMA-latency.text` - Latency analysis notes
- `prompts.txt` - Development prompts and debugging notes

## Code Organization Patterns

### Common Structure in Demo Files

1. **Header Includes**
   - Standard C++ headers
   - libfabric headers (`rdma/fabric.h`, `rdma/fi_*.h`)
   - CUDA headers (when applicable)

2. **Macros**
   - `CHECK()` - Assertion macro
   - `FI_CHECK()` - libfabric error checking macro

3. **Constants**
   - Buffer sizes (e.g., `kMessageBufferSize`, `kMemoryRegionSize`)
   - Queue sizes (e.g., `kCompletionQueueReadCount`)

4. **Data Structures**
   - `EfaAddress` - Network address wrapper
   - `Buffer` - Memory buffer management
   - `Network` - libfabric abstraction layer
   - `RdmaOp` variants - Operation descriptors

5. **Main Functions**
   - `ServerMain()` - Server-side logic
   - `ClientMain()` - Client-side logic
   - `main()` - Entry point with mode selection

### Naming Conventions

- **Constants**: `kCamelCase` (e.g., `kMessageSize`)
- **Classes/Structs**: `PascalCase` (e.g., `EfaAddress`, `RdmaChannel`)
- **Functions**: `PascalCase` for public, `camelCase` for private
- **Variables**: `snake_case` with trailing underscore for members (e.g., `data_`, `size_`)
- **Enums**: `PascalCase` with `k` prefix for values (e.g., `RdmaOpType::kRecv`)

### Error Handling Pattern

```cpp
FI_CHECK(fi_operation(...));  // Exits on error with message
```

### Resource Management

- RAII wrappers (e.g., `FabricResource<T>`)
- Move semantics for resource transfer
- Explicit cleanup in destructors

## Dependencies Between Files

- All demo files are standalone executables
- Common patterns are duplicated across files (intentional for learning)
- No shared library or header files
- Each demo can be compiled and run independently
