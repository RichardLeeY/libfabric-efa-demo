# RDMA Write Hybrid Latency Test

This test measures RDMA write latency using a **hybrid measurement approach** that combines memory polling and completion event monitoring to provide comprehensive latency analysis.

## Overview

The `rdma_write_hybrid_test` program implements a sophisticated client-server model where:
- **Server**: Uses dual measurement threads to detect both memory changes and completion events
- **Client**: Performs high-speed RDMA writes with immediate data and measures write completion latency

## Key Features

- **Hybrid Measurement**: Combines memory polling and completion event monitoring
- **High Throughput**: Sends 10,000 RDMA writes over 10 seconds (1,000 writes/sec)
- **Immediate Data**: Uses RDMA write with immediate data for enhanced completion tracking
- **Dual Server Threads**: Separate threads for memory detection and completion processing
- **EFA Optimization**: 4096-entry completion queue for high-speed operations
- **Statistical Analysis**: Provides comprehensive latency percentile analysis

## Usage

### Build
```bash
make rdma_write_hybrid_test
```

### Server Mode
```bash
./build/rdma_write_hybrid_test <port>
```

Example:
```bash
./build/rdma_write_hybrid_test 57891
```

### Client Mode
```bash
./build/rdma_write_hybrid_test <server_ip> <port>
```

Example:
```bash
./build/rdma_write_hybrid_test 172.31.80.253 57891
```

## Hybrid Measurement Architecture

### Server-Side Dual Measurement
1. **Memory Polling Thread**: Continuously monitors target memory for data changes
2. **Completion Polling Thread**: Monitors EFA completion queue for completion events
3. **Timestamp Correlation**: Correlates memory detection with completion notifications

### Three Critical Timestamps
- **Client Send Time**: When client initiates RDMA write (`client_send_timestamp`)
- **Server Memory Time**: When server detects data in memory (`server_memory_timestamp`)  
- **Server Completion Time**: When completion event fires (`server_completion_timestamp`)

## How It Works

### Initialization Phase
1. **EFA Setup**: Both sides initialize libfabric with EFA provider and 4096-entry completion queues
2. **Address Exchange**: EFA addresses exchanged via UDP on specified port
3. **Memory Registration**: Server registers receive buffer, client registers send buffer
4. **Memory Info Exchange**: Server sends memory region details via UDP on port+1

### High-Speed Write Phase
1. **Client Threads**:
   - **Push Thread**: Queues 10,000 write requests over 10 seconds
   - **Write Thread**: Executes RDMA writes with immediate data and sequence numbers
   - **Completion Thread**: Aggressively polls for write completions (1μs intervals)

2. **Server Threads**:
   - **Memory Thread**: Polls memory for sequence number changes
   - **Completion Thread**: Processes completion events with immediate data

### Advanced Flow Control
- **Aggressive Completion Polling**: 1μs polling intervals to prevent queue overflow
- **Retry Logic**: Automatic retry with immediate polling when send queue is full
- **Large Completion Queue**: 4096 entries to handle burst traffic

## Configuration

Key parameters:
```cpp
constexpr size_t kMessageSize = 64;              // Message size
constexpr size_t kTotalMessageCount = 10000;     // Total RDMA writes
constexpr uint32_t kSendDurationMs = 10000;      // 10 seconds duration
constexpr size_t kCompletionQueueReadCount = 16; // Batch completion reads
constexpr uint32_t kSpscQueueSize = 10000;       // Internal queue size
```

## Expected Output

### Server Output
```
--------------------------------
domain: rdmap201s0-rdm
nic: rdmap201s0
fabric: efa
link: 60Gbps
--------------------------------
Self EFA address = fe8000000000000010b24dfffee76a9f01000000959d02f0000000000000000
[DEBUG] Server: Memory registered, key=3145730
[DEBUG] Server: Memory region exchange completed (16 bytes sent)

--- Server Memory Detection: First 5 Messages ---
Msg#    Client Send (μs)   Memory Detect (μs)   E2E Latency (μs)
-----------------------------------------------------------------
   1         1767625092343672.750    1767625092345948.500       2275.348
   2         1767625092344781.500    1767625092346798.500       2017.000
   3         1767625092345800.000    1767625092347816.500       2016.500
   4         1767625092346788.000    1767625092348802.250       2014.250
   5         1767625092347787.750    1767625092349804.250       2016.500

--- Server Completion Events: First 5 Messages ---
Msg#      Memory (μs)        Completion (μs)     Delta (μs)
----------------------------------------------------------
   1    1767625092345948.500  1767625092345950.000      1.500
   2    1767625092346798.500  1767625092346815.750     17.250
   3    1767625092347816.500  1767625092347833.000     16.500
   4    1767625092348802.250  1767625092348816.750     14.500
   5    1767625092349804.250  1767625092349820.750     16.500

=== Hybrid Server-Side RDMA Write Latency Analysis ===
Total measurements: 10000

End-to-End Latency (Client Send → Server Memory Detection):
  Min: 1.234 μs
  Max: 15.678 μs
  P50: 2.345 μs
  P90: 4.567 μs
  P99: 8.901 μs

Completion Event vs Memory Detection Delta:
  Min: 0.500 μs
  Max: 25.432 μs
  P50: 1.750 μs
  P90: 3.456 μs
  P99: 12.345 μs
```

### Client Output
```
[DEBUG] Client: Received memory region info - addr=338476336, key=3145730
Started write thread with immediate data
Starting to send 10000 RDMA writes over 10 seconds
Write thread completed - sent 10000 RDMA writes with immediate data
Client completed sending all RDMA writes with immediate data
```

## Key Measurements

### End-to-End Latency
**Client Send → Server Memory Detection**
- True network + EFA latency
- Measures actual data arrival time
- Typically 1-5 μs for EFA

### Completion Delta  
**Memory Detection → Completion Event**
- EFA completion notification overhead
- Usually 1-20 μs additional delay
- Shows completion queue processing time

### Write Completion (Client-side)
- Client-side write operation completion
- Includes send queue management
- Affected by completion polling frequency

## Advantages of Hybrid Approach

1. **True Latency Measurement**: Memory polling captures actual data arrival
2. **Completion Overhead Analysis**: Separates network latency from completion processing
3. **High Throughput Capable**: Handles 1000+ writes/second with large completion queues
4. **Comprehensive Statistics**: Multiple latency perspectives in single test

## Troubleshooting

### Common Issues
1. **"Resource temporarily unavailable"**: Send queue full - increase completion polling frequency
2. **"Flags not supported"**: Use `FI_TRANSMIT | FI_RECV` for EFA RDM endpoints
3. **UDP exchange failures**: Check security groups allow UDP on port and port+1
4. **Memory registration failures**: Ensure sufficient memory and EFA driver loaded

### Performance Tuning
- **Completion Queue Size**: Increase from 1024 to 4096+ for high throughput
- **Polling Frequency**: Balance CPU usage vs latency (1μs for maximum performance)
- **Batch Processing**: Read multiple completions per poll cycle
- **Thread Affinity**: Pin threads to specific CPU cores for consistent performance

## Use Cases

This hybrid test is ideal for:
- **Latency Analysis**: Understanding true RDMA write latency vs completion overhead
- **High-Throughput Testing**: Validating performance under sustained load
- **EFA Optimization**: Tuning completion queue sizes and polling strategies
- **Production Validation**: Testing real-world RDMA write patterns
- **Performance Comparison**: Comparing different RDMA approaches and configurations
