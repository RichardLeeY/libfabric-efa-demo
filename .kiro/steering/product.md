# Product Overview

This is a libfabric-efa-demo repository showcasing high-performance RDMA (Remote Direct Memory Access) communication using AWS EFA (Elastic Fabric Adapter) and the libfabric API.

## Purpose

Demonstrates various RDMA patterns and optimizations for achieving high-throughput, low-latency network communication on AWS infrastructure, with a focus on GPU memory transfers and CPU-to-CPU data movement.

## Key Features

- RDMA send/receive operations using libfabric
- RDMA write operations to CPU memory
- Direct memory access patterns with EFA and EFA-direct providers
- Performance optimization techniques (warmup, multithreading, pinning, sharding, batching, lazy operations)
- Client-server architecture examples
- Zero-copy, kernel-bypass data transfers

## Target Use Cases

- High-performance computing (HPC) workloads
- GPU-to-GPU memory transfers
- Low-latency distributed systems
- Network-intensive applications requiring direct memory access
- AWS EFA-enabled instance communication

## Blog Reference

Journey to 3200 Gbps: High-Performance GPU Memory Transfer on AWS
https://www.perplexity.ai/hub/blog/high-performance-gpu-memory-transfer-on-aws
