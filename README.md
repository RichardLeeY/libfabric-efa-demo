# libfabric-efa-demo

Blog Post: [Journey to 3200 Gbps: High-Performance GPU Memory Transfer on AWS](https://www.perplexity.ai/hub/blog/high-performance-gpu-memory-transfer-on-aws)

# Build Instructions

Install GDRCopy, libfabric, and fabtests inside `build/` folder:

```bash
bash ./install-deps.sh
```

Build the demo code:

```bash
make
```

See the comments at the top of each source file for instructions and examples of each demo.
