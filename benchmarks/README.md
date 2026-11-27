# Benchmarks

The benchmark client measures end-to-end `Set` throughput against a running node.
It includes gRPC serialization and the Raft commit path, so results vary with cluster
size, network latency, storage, build type, and machine load. The current benchmark is
a basic throughput tool, not a comprehensive performance or capacity qualification.

Build with CMake, start a node, then run:

```text
./build/kvstore_benchmark localhost:50051 1000
```

For useful comparisons, record the commit, compiler/build type, node count, hardware,
network, operation count, storage, and run conditions. Repeat runs and report latency
percentiles as well as throughput before using results to set capacity expectations.
The benchmark reports sequential end-to-end operation latency, including p50, p95,
p99, and maximum milliseconds. Production performance and scale testing remain open
work; see [../docs/PRODUCTION-READINESS.md](../docs/PRODUCTION-READINESS.md).
