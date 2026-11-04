# Distributed Associative Computing Architecture (DACA)

**A Preliminary Computational Reuse Model and Software Benchmark**

## 📖 Overview
Modern computing architectures waste significant energy recalculating identical or structurally similar mathematical operations. **DACA** is a proposed hardware architecture that addresses this by caching *mathematical relationships* (execution tables) rather than just exact data values. 

Through a series of multi-threaded C++ benchmarks, this research models DACA's performance across memory latencies, dynamic workloads, and multi-core contention. By distributing memory across a parallel dataflow highway and utilizing a hybrid algebraic-fallback policy, DACA bypasses heavy computations while maintaining 100% mathematical correctness.

## 🏆 Key Published Results
*(All benchmarks are verified against a Naive baseline and an optimized Exact-Result Cache)*

*   **7.02x Speedup over Exact Caching:** In the multithreaded highway macrobenchmark (200,000 requests, 8 threads), DACA bypassed 97.2% of heavy computational kernels, finishing in 62.9 ms compared to the Exact Cache's 441.8 ms.
*   **24.64x Speedup over Naive Compute:** The same multi-threaded workload showed DACA completing the task 24.6x faster than a standard CPU calculating everything from scratch.
*   **Compositional Reuse:** DACA successfully extracted non-contiguous subsets and assembled novel combinations from disparate memory records, achieving up to a 15.7x speedup over the baseline for fragmented data.
*   **Dynamic (Warped) Data:** In a shuffled, randomized workload simulating changing environmental states, DACA sustained a 5.67x speedup over standard caching.
*   **Zero-Contention Topology:** The 64-node sharded highway model eliminates global mutex bottlenecks, scaling efficiently across 8 threads.

## 📂 Repository Structure & Benchmarks

The C++ files are standalone. Build commands (`g++ -O3 -pthread`) and limitations are documented alongside each experiment.

*   **[Multithreaded Highway Macrobenchmark](README_daca_highway_macrobenchmark.md):** Compares direct computation, a centralized exact cache, and a 64-node sharded exact/relationship cache on a 200k-request shuffled workload.
*   **[Compositional Relationship Benchmark](README_daca_composition_benchmark.md):** Tests contiguous, non-contiguous, and cross-record reuse for addition.
*   **[Software Chunk Benchmark](README_daca_software_benchmark.md):** Compares direct computation, conventional atomic caching, and DACA-style contiguous segment reuse.
*   **[CPU Memory Benchmark](README_cpu_memory_benchmark.md):** An earlier software benchmark of chunk and request reuse policies.
*   **[Single-Node Chip Timing Simulation](README_daca_chip_architecture_sim.md):** A discrete, assumption-driven model of queues, cores, memory lookup, composition, and lanes.
*   **[Contention Microbenchmark](daca_contention_benchmark.cpp):** A synchronization-only test measuring mutex versus atomic-counter overhead.
*   **[Menu Runner](daca_benchmark_menu.cpp):** A single-file entry point to compile and run all benchmarks from a single menu.

## ⚠️ Important Interpretation
The benchmark named "DACA" is a **software model** of selected reuse rules. It does not model physical layout, power, real interconnect latency, transistor behavior, or measured silicon. Results depend on the synthetic workload, data structures, compiler, and host machine. Treat them as prototype results and use the correctness checks and stated limitations when comparing them.

## 📜 License
*   The C++ source code is licensed under the **MIT License**.
*   The research paper and written documentation are licensed under **CC BY 4.0**.
