![C++](https://img.shields.io/badge/Language-C%2B%2B17-blue.svg)
![License](https://img.shields.io/badge/License-MIT-green.svg)
![Tests](https://img.shields.io/badge/Tests-10%2F10%20PASS-brightgreen.svg)
![Platform](https://img.shields.io/badge/Platform-Android%20%7C%20Linux-lightgrey.svg)

Distributed Associative Computing Architecture (DACA)

A Preliminary Computational Reuse Model and Software Benchmark

📖 Overview

Modern computing architectures waste significant energy recalculating structurally similar computational operations. DACA is a proposed hardware architecture that addresses this by caching computational relationships and state transitions (execution tables) rather than just exact data values.

While the initial benchmarks in this repository focus on algebraic arithmetic as a proof-of-concept, the DACA architecture is inherently data-agnostic. Because it caches dependencies and execution graphs, it can be applied to string parsing, state machines, DOM layouts, and AI inference—any workload where the same logical transitions are repeated.

Through a series of multi-threaded C++ benchmarks, this research models DACA's performance across memory latencies, dynamic workloads, and multi-core contention. By distributing memory across a parallel dataflow highway and utilizing a hybrid algebraic-fallback policy, DACA bypasses heavy computations while maintaining 100% mathematical correctness.

🏆 Key Published Results

(Verified against Naive baselines and highly optimized Exact-Result Caches)

    38x Speedup on Compositional Reuse: In a fair compositional comparison, an optimized Exact Subset Cache suffered from a combinatorial explosion (1,504 ms training, 262 KB memory, 83 hits). DACA achieved 315 hits using only 15 KB of memory and 0.18 ms of training time, resulting in a 38x query speedup.

    7.02x Speedup over Exact Caching: In the multithreaded highway macrobenchmark (200,000 requests, 8 threads), DACA bypassed 97.2% of heavy computational kernels, finishing in 62.9 ms compared to the Exact Cache's 441.8 ms.

    24.64x Speedup over Naive Compute: The same multi-threaded workload showed DACA completing the task 24.6x faster than a standard CPU calculating everything from scratch.

    10/10 PASS on the Adaptive Logic Suite: Across 10 tests covering versioning, invalidation, graph composition, and conflict validation, DACA maintained 100% mathematical correctness with an overall suite speedup of 4.53x.

🛠️ The Benchmark Suites

The repository contains two main C++ entry points:

    Adaptive Unified Benchmark (daca_adaptive_unified_benchmark.cpp): A 10-test standalone suite evaluating DACA's logical inference, dynamic invalidation, and algebraic composition against Naive and Exact-Cache baselines.

🚀 Build and Run

To run the 10-test logic suite:
g++ -O3 -pthread -std=c++17 daca_adaptive_unified_benchmark.cpp -o daca_suite
./daca_suite

To run the hardware/memory benchmark menu:
g++ -O3 -pthread -std=c++17 daca_benchmark_menu.cpp -o daca_menu
./daca_menu

⚠️ Important Interpretation & Limitations

The benchmark named "DACA" is a software model of selected reuse rules. It does not model physical layout, power, real interconnect latency, transistor behavior, or measured silicon. Results depend on the synthetic workload, data structures, compiler, and host machine.

    Commutative Limitation: This benchmark evaluates additive relationships (which are associative and commutative). Future work must implement operation-aware dependency graphs for non-commutative operations (e.g., subtraction, matrix multiplication), where operand order must be strictly preserved.

    Memory Accounting: Logical memory byte counts are estimates; allocator overhead is not included.

📚 Citation and Prior Art

This repository serves as the public Prior Art for the DACA architecture. The architecture, experimental design, and mathematical analysis were conceived and implemented by Shikhali Huseynli in 2026.

If you reference this work, or use the concepts of relationship-based caching, the sharded highway, or the hybrid non-linear fallback policy, please cite the author appropriately.

    Author: Shikhali Huseynli

    GitHub: @shixeli231-creator

    Year: 2026

License:
The C++ source code is licensed under the MIT License.
The research paper, documentation, and architectural concepts are licensed under Creative Commons Attribution 4.0 (CC BY 4.0). You are free to share and adapt this material for any purpose, provided you give appropriate credit to the original author.
