# 16_write_cpu.cpp - RDMA CPU Memory Write Demo

## Overview

This demo showcases RDMA (Remote Direct Memory Access) write operations to CPU memory using AWS EFA (Elastic Fabric Adapter) and libfabric. It implements a client-server architecture where the server generates random data and writes it directly to the client's CPU memory using RDMA operations.

## Architecture

The demo follows a client-server model:
- **Server**: Generates random data and performs RDMA writes to client memory
- **Client**: Registers memory regions and receives data via RDMA writes

### Memory Layout

#### Server Side Memory
```
┌─────────────────────────────────────────────────────────────┐
│                    Server Memory Layout                     │
├─────────────────────────────────────────────────────────────┤
│ Message Buffers (2 × 8KB)                                  │
│ ┌─────────────┐ ┌─────────────┐                           │
│ │   buf1      │ │   buf2      │  (for CONNECT/REQUEST)    │
│ │   8KB       │ │   8KB       │                           │
│ └─────────────┘ └─────────────┘                           │
├─────────────────────────────────────────────────────────────┤
│ CPU Buffers (2 × 16MB)                                     │
│ ┌─────────────────────────────────────────────────────────┐ │
│ │              cpu_buf1 (16MB)                            │ │
│ │ Random data for client MR0                              │ │
│ └─────────────────────────────────────────────────────────┘ │
│ ┌─────────────────────────────────────────────────────────┐ │
│ │              cpu_buf2 (16MB)                            │ │
│ │ Random data for client MR1                              │ │
│ └─────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

#### Client Side Memory
```
┌─────────────────────────────────────────────────────────────┐
│                    Client Memory Layout                     │
├─────────────────────────────────────────────────────────────┤
│ Message Buffer (8KB)                                        │
│ ┌─────────────┐                                             │
│ │   buf1      │  (for CONNECT/REQUEST messages)            │
│ │   8KB       │                                             │
│ └─────────────┘                                             │
├─────────────────────────────────────────────────────────────┤
│ CPU Memory Regions (2 × 16MB)                              │
│ ┌─────────────────────────────────────────────────────────┐ │
│ │              cpu_buf1 (MR0 - 16MB)                     │ │
│ │ ┌─────┐ ┌─────┐ ┌─────┐     ┌─────┐                   │ │
│ │ │Page0│ │Page1│ │Page2│ ... │PageN│  (1MB pages)      │ │
│ │ └─────┘ └─────┘ └─────┘     └─────┘                   │ │
│ │              ↑ RDMA Write Target                       │ │
│ └─────────────────────────────────────────────────────────┘ │
│ ┌─────────────────────────────────────────────────────────┐ │
│ │              cpu_buf2 (MR1 - 16MB)                     │ │
│ │ ┌─────┐ ┌─────┐ ┌─────┐     ┌─────┐                   │ │
│ │ │Page0│ │Page1│ │Page2│ ... │PageN│  (1MB pages)      │ │
│ │ └─────┘ └─────┘ └─────┘     └─────┘                   │ │
│ │              ↑ RDMA Write Target                       │ │
│ └─────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## Key Components

### Core Data Structures

#### EfaAddress
- Represents a 32-byte EFA network address
- Provides string conversion and parsing utilities
- Used for peer identification and connection establishment

#### Buffer
- Simplified memory buffer without alignment requirements
- Handles memory allocation and cleanup
- Uses standard malloc/free for memory management

#### Network
- Main networking abstraction wrapping libfabric operations
- Manages fabric, domain, completion queue, address vector, and endpoint
- Provides high-level RDMA operations (send, receive, write)

#### RDMA Operations
- `RdmaRecvOp`: Receive operation structure
- `RdmaSendOp`: Send operation structure  
- `RdmaWriteOp`: RDMA write operation structure
- `RdmaRemoteWriteOp`: Remote write completion tracking

### Application Messages

#### AppConnectMessage
- Initial handshake message from client to server
- Contains client address and memory region information
- Includes remote keys (rkeys) for RDMA access

#### AppRandomFillMessage
- Request message for random data generation
- Specifies seed, page size, number of pages
- Contains page indices for scattered writes

## Workflow

### Server Side
1. **Initialization**
   - Opens libfabric network interface
   - Allocates and registers two separate CPU memory buffers (16MB each)
   - Sets up message buffers for communication

2. **Connection Handling**
   - Receives CONNECT message from client
   - Extracts client address and memory region info
   - Adds client to address vector

3. **Data Generation & Transfer**
   - Receives RandomFill request with parameters
   - Generates random data using specified seed
   - Performs multiple RDMA writes to client memory
   - Uses immediate data for completion notification

### Client Side
1. **Initialization**
   - Opens libfabric network interface
   - Allocates and registers two CPU memory buffers
   - Connects to server using provided address

2. **Memory Registration**
   - Sends CONNECT message with memory region details
   - Provides remote keys for server access

3. **Request & Verification**
   - Sends RandomFill request with parameters
   - Waits for RDMA write completion
   - Verifies received data against expected values

## Key Features

### RDMA Write Operations
- Direct memory-to-memory transfers without CPU involvement
- Uses immediate data for completion signaling
- Handles multiple memory regions and scattered writes

### Error Handling
- Implements retry logic with exponential backoff for EAGAIN errors
- Comprehensive error checking with libfabric operations
- Graceful cleanup of resources

### Memory Management
- Simplified memory allocation without alignment
- Memory region registration for RDMA access
- Automatic cleanup via RAII patterns

### Debug Features
- Detailed page index tracking and validation
- Boundary checking for RDMA writes
- Comprehensive logging of memory operations
- Client-side configuration display

## Usage

### Server Mode
```bash
./build/16_write_cpu
```
Server displays its address and waits for client connections.

### Client Mode
```bash
./build/16_write_cpu <server_address> [page_size num_pages]
```
- `server_address`: 64-character hex string from server output
- `page_size`: Size of each page (default: 1MB)
- `num_pages`: Number of pages to transfer (default: 8)

## Performance Characteristics

- **Memory Size**: 16MB per memory region
- **Default Transfer**: 8MB (8 pages × 1MB each)
- **Alignment**: No special alignment requirements
- **Concurrency**: Supports multiple concurrent RDMA writes
- **Debug Output**: Extensive debugging information for troubleshooting

## Technical Details

### libfabric Configuration
- Provider: EFA (Elastic Fabric Adapter)
- Endpoint Type: RDM (Reliable Datagram)
- Capabilities: MSG, RMA, LOCAL_COMM, REMOTE_COMM
- Memory Registration: LOCAL, VIRT_ADDR, ALLOCATED, PROV_KEY

### Completion Handling
- Uses data completion queue format
- Handles different completion types (RECV, SEND, WRITE, REMOTE_WRITE)
- Implements callback-based completion processing

### Data Verification
- Uses MT19937-64 random number generator
- Deterministic seed-based generation for verification
- Compares expected vs actual data after transfer

## Error Scenarios

The demo handles various error conditions:
- Network resource exhaustion (EAGAIN)
- Invalid addresses or parameters
- Memory registration failures
- Completion queue errors

## Dependencies

- libfabric (RDMA communication)
- EFA provider (AWS Elastic Fabric Adapter)
- C++17 standard library
- POSIX threading support

## Data Flow Sequence Diagram (libfabric API Level)

```
Client                          EFA Network                     Server
  |                                 |                             |
  |-- fi_getinfo() --------------->|                             |
  |<-- info returned --------------|                             |
  |                                |<-- fi_getinfo() ------------|  
  |                                |-- info returned ----------->|
  |                                |                             |
  |-- fi_fabric() --------------->|                             |
  |-- fi_domain() --------------->|                             |
  |-- fi_cq_open() -------------->|                             |
  |-- fi_av_open() -------------->|                             |
  |-- fi_endpoint() ------------->|                             |
  |-- fi_ep_bind() -------------->|                             |
  |-- fi_enable() --------------->|                             |
  |                                |<-- fi_fabric() -------------|  
  |                                |<-- fi_domain() -------------|  
  |                                |<-- fi_cq_open() ------------|  
  |                                |<-- fi_av_open() ------------|  
  |                                |<-- fi_endpoint() -----------|  
  |                                |<-- fi_ep_bind() ------------|  
  |                                |<-- fi_enable() -------------|  
  |                                |                             |
  |-- fi_mr_regattr() ----------->|                             |  // Register cpu_buf1
  |-- fi_mr_regattr() ----------->|                             |  // Register cpu_buf2
  |-- fi_mr_regattr() ----------->|                             |  // Register msg_buf
  |                                |<-- fi_mr_regattr() ---------|  // Register cpu_buf1
  |                                |<-- fi_mr_regattr() ---------|  // Register cpu_buf2
  |                                |<-- fi_mr_regattr() ---------|  // Register msg_buf1
  |                                |<-- fi_mr_regattr() ---------|  // Register msg_buf2
  |                                |                             |
  |                                |<-- fi_recvmsg() ------------|  // Post recv for CONNECT
  |                                |<-- fi_recvmsg() ------------|  // Post recv for REQUEST
  |                                |                             |
  |-- fi_av_insert() ------------>|                             |  // Add server address
  |                                |                             |
  |== CONNECT MESSAGE EXCHANGE ===|=============================|
  |                                |                             |
  |-- fi_sendmsg() -------------->|                             |  // Send CONNECT
  |   (AppConnectMessage)          |                             |
  |   - client_addr                |                             |
  |   - MR[0]: addr, size, rkey    |                             |
  |   - MR[1]: addr, size, rkey    |                             |
  |                                |-- CONNECT delivered ------->|
  |                                |                             |-- fi_cq_read() // Process CONNECT
  |                                |<-- fi_av_insert() ----------|  // Add client address
  |<-- send completion ------------|                             |
  |-- fi_cq_read() // Send done    |                             |
  |                                |                             |
  |== RANDOM FILL REQUEST =========|=============================|
  |                                |                             |
  |-- AddRemoteWrite() ----------->|                             |  // Setup completion handler
  |                                |                             |
  |-- fi_sendmsg() -------------->|                             |  // Send RandomFill request
  |   (AppRandomFillMessage)       |                             |
  |   - remote_context: 0x123      |                             |
  |   - seed: 0xb584...            |                             |
  |   - page_size: 1048576         |                             |
  |   - num_pages: 8               |                             |
  |                                |-- REQUEST delivered ------->|
  |                                |                             |-- fi_cq_read() // Process REQUEST
  |<-- send completion ------------|                             |
  |-- fi_cq_read() // Send done    |                             |
  |                                |                             |
  |== DATA GENERATION & RDMA WRITES |============================|
  |                                |                             |
  |                                |                             |-- RandomBytes() // Generate data
  |                                |                             |
  |                                |                             |-- fi_writemsg() // Write to MR0, Page0
  |                                |                             |   (imm_data=0)
  |                                |<-- RDMA WRITE --------------|   
  |<-- data written to cpu_buf1 ---|                             |
  |                                |                             |-- fi_writemsg() // Write to MR0, Page1
  |                                |<-- RDMA WRITE --------------|   (imm_data=0)
  |<-- data written to cpu_buf1 ---|                             |
  |                                |                             |     ...
  |                                |                             |-- fi_writemsg() // Write to MR1, Page0
  |                                |<-- RDMA WRITE --------------|   (imm_data=0)
  |<-- data written to cpu_buf2 ---|                             |
  |                                |                             |     ...
  |                                |                             |-- fi_writemsg() // FINAL Write
  |                                |<-- RDMA WRITE --------------|   (imm_data=0x123)
  |<-- data written to cpu_buf2 ---|                             |
  |<-- REMOTE_WRITE completion ----|                             |
  |   (cqe.data=0x123)             |                             |
  |-- fi_cq_read() // Final completion                           |
  |                                |                             |-- fi_cq_read() // Write completions
  |                                |                             |
  |== DATA VERIFICATION ===========|=============================|
  |                                |                             |
  |-- RandomBytes() // Generate expected                         |
  |-- memcmp() // Verify data                                    |
  |-- printf("Data is correct")                                  |
  |                                |                             |
```

### Key libfabric API Calls Explained:

**Initialization Phase:**
- `fi_getinfo()`: Query available fabric providers and capabilities
- `fi_fabric()`, `fi_domain()`: Create fabric and domain objects
- `fi_cq_open()`, `fi_av_open()`: Create completion queue and address vector
- `fi_endpoint()`, `fi_ep_bind()`, `fi_enable()`: Create and activate endpoint

**Memory Registration:**
- `fi_mr_regattr()`: Register memory regions for RDMA access
- Returns memory keys (rkeys) for remote access

**Communication Setup:**
- `fi_recvmsg()`: Post receive buffers (server side)
- `fi_av_insert()`: Add peer addresses to address vector

**Message Exchange:**
- `fi_sendmsg()`: Send messages (CONNECT, RandomFill request)
- `fi_cq_read()`: Poll completion queue for operation completions

**RDMA Operations:**
- `fi_writemsg()`: Perform RDMA write to remote memory
- Uses remote address, length, and rkey from CONNECT message
- Final write includes immediate data for completion notification

**Completion Handling:**
- Server gets local write completions
- Client gets remote write completion with immediate data
- `FI_REMOTE_WRITE` flag indicates remote write completion

The sequence shows how libfabric enables zero-copy, kernel-bypass data transfer directly between application memory regions.


**Reference**
https://www.slideshare.net/slideshow/ofi-libfabric-tutorial/55227620