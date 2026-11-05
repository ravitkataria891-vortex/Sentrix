# Sentrix: A High-Performance Engine for Eliminating Redundant Computation at Scale

> *Detects repeated requests and instantly serves the answer already known — cutting redundant compute and API cost in high-traffic and AI-driven systems.*

## Overview

Most high-throughput systems waste compute repeating requests they have already seen and recomputing answers they already know. **Sentrix** is a lightweight, single-threaded **C++17** request-processing pipeline that answers two questions in near-constant time before any expensive downstream work runs:

1. **"Have I seen this before?"** — Answered in `O(k)` time by a space-efficient, probabilistic **Bloom Filter** (`Stage 1`).
2. **"Do I already know the answer?"** — Answered in `O(1)` time by a strictly memory-bounded **LRU Cache** (`Stage 2`, Hash Map + custom Doubly Linked List).

Sentrix is structured around a generic, stage-agnostic pipeline (`DROP`, `RESOLVED`, `CONTINUE`) intentionally scoped to a verified, benchmarked two-stage core, while preserving clean extension points for Stage 0 query normalization, Aho–Corasick payload scanning, concurrency, and persistence.

---

## Practical Use Cases

- **AI Prompt / Query Caching (Primary Use Case):** `BloomMode::GateCache` flags whether an incoming prompt has been seen before (skipping cache lookups on unseen prompts), while the `LRUCache` immediately returns cached model responses instead of re-invoking expensive LLM/API inference calls.
- **API Request Deduplication & Idempotency:** Suppresses duplicate requests caused by client retries or double-submitted forms (`BloomMode::DropDuplicates`) and serves cached responses for recently repeated calls.
- **Duplicate Event / Log Ingestion Filtering:** In at-least-once delivery pipelines, re-delivered events are dropped in `O(k)` time by the Bloom Filter or resolved from cache without repeating downstream processing.
- **Market Tick Deduplication:** Filters duplicate ticks arriving from redundant feed sources while keeping the latest price per symbol accessible in `O(1)` time.

---

## System Architecture & Technical Specifications

Incoming payloads pass through an ordered chain of `Stage` objects (`src/pipeline.hpp`). Each stage returns `StageStatus::DROP`, `StageStatus::RESOLVED`, or `StageStatus::CONTINUE`, and execution halts at the first non-`CONTINUE` result:

```text
Incoming Payload
       │
       ▼
┌──────────────────────────────────────────────────────────────┐
│ Stage 1: BloomStage (std::vector<bool>, FNV-1a + MurmurHash3)│
│  • DropDuplicates mode + seen  ──► HALT (DROP)               │
│  • GateCache mode + unseen     ──► mark definitely_new       │
└──────────────────────────────┬───────────────────────────────┘
                               │ CONTINUE
                               ▼
┌──────────────────────────────────────────────────────────────┐
│ Stage 2: CacheStage (Hash Map + unique_ptr Doubly Linked LRU)│
│  • definitely_new == true      ──► skip map lookup, CONTINUE │
│  • Cache hit                   ──► HALT (RESOLVED + answer)  │
│  • Cache miss                  ──► CONTINUE to backend       │
└──────────────────────────────────────────────────────────────┘
```

| Domain | Implementation Strategy |
|---|---|
| **Core Language** | **C++17** (`-Wall -Wextra -Wpedantic`, Release LTO/IPO), chosen for deterministic memory layout and low-level pointer control. |
| **Memory Management** | Forward `std::unique_ptr<Node>` links own LRU nodes directly inside the doubly linked list; `prev` pointers and `std::unordered_map<std::string_view, Node*>` hold non-owning raw pointers for `O(1)` lookup with zero reference cycles, zero duplicate key strings, and iterative stack-safe destruction. |
| **Data Structures** | Runtime-sized `std::vector<bool>` Bloom Filter probed via Kirsch–Mitzenmacher double hashing over 32-bit **FNV-1a** and **MurmurHash3**; Hash Map + Doubly Linked List with evicted tail-node recycling for the LRU Cache. |
| **Saturation Handling** | Computed capacity flush threshold `n_max = floor(-(m / k) * ln(1 - p^(1 / k)))` derived from bit count `m`, hash count `k`, and target FPR `p`. Counts only distinct bit-changing insertions and flushes before permanent saturation (`100%` false-positive state) can occur. |
| **Concurrency** | Single-threaded by design to keep pointer ownership and latency deterministic; concurrent sharding/locking is scoped as future work. |

---

## Benchmarks & Real-World Cost Evaluation

All benchmarks are reproducible via `./build/sentrix_benchmark` (deterministic seed, median of 3 runs, isolated child-process Peak RSS measurement via Win32 `GetProcessMemoryInfo` / POSIX `/proc` & `getrusage`).

### 1. End-to-End Pipeline Use-Case & API Cost Benchmark (50,000 Requests Each)

To evaluate Sentrix beyond synthetic container microbenchmarks, `src/benchmark.cpp` simulates three realistic 50,000-request workloads:

1. **Busy AI Help Desk / Chatbot (`GateCache`):** 500 recurring FAQ prompts mixed with 19,500 one-off user tickets (`60%` repeat traffic), guarded by a Bloom Filter and a bounded **1,000-slot LRU notepad**, with simulated LLM/backend cost (`500` hash rounds + **`$0.002` per backend API call**).
2. **Double-Click / Event Retry Storm (`DropDuplicates`):** 30,000 unique order submissions + 20,000 duplicate client retries (`40%` duplicates) dropped immediately at Stage 1.
3. **99% Unique + Trivial Lookup (Negative Control):** 49,500 unique requests (`99%` unique) with near-zero backend cost, illustrating when pipeline bookkeeping is **not** worth adding.

| Scenario (50,000 Requests) | Mode | Without Sentrix | With Sentrix | Speedup | Backend Calls Executed | Sentrix Memory |
|---|---|---:|---:|---:|---:|---:|
| **1. AI Help Desk (60% repeats)** | `GateCache` (1K LRU) | 594.85 ms | **257.17 ms** | **2.31x faster** | **24,877 / 50,000** | **175.28 KiB** |
| **2. Double-Click Retries (40% dup)** | `DropDuplicates` | 732.85 ms | **462.18 ms** | **1.59x faster** | **29,937 / 50,000** | **35.14 KiB** |
| **3. 99% Unique + Cheap (Not worth it)** | `GateCache` (1K LRU) | **5.24 ms** | 49.19 ms | **0.11x (slower)** | 49,953 / 50,000 | 211.89 KiB |

#### Cost & Operational Impact
- **50.25% AI API Cost Reduction (`$100.00` → `$49.75`):** Without Sentrix, all 50,000 prompts hit the model API (`50,000 × $0.002 = $100.00`). With Sentrix holding just the 1,000 most recent answers (`175.28 KiB` combined Bloom + LRU memory), **25,123 repeated queries** are served instantly from memory—cutting API spend to **`$49.75` (`$50.25` saved per 50k requests)** while cutting latency by more than half (**2.31x speedup**).
- **20,063 Duplicate Executions Prevented in `35.14 KiB`:** During retry storms, Stage 1 drops duplicate order/event submissions in `O(k)` time without storing payload strings.
- **Clear Trade-Off Boundary:** When traffic has almost no repetition (`99%` unique) and downstream work is already trivial, Sentrix runs slower (`5.24 ms` vs `49.19 ms`), confirming it should be deployed only where requests repeat and downstream work or memory is expensive.

---

### 2. Standard-Library Baseline Comparison (10K / 100K / 1M Payloads)

Deterministic **80% unique / 20% duplicate** stream (`target FPR = 1.00%`, `k = 7`, bounded `LRUCache` capacity = `50,000`) compared against `std::unordered_set` (deduplication) and an unbounded `std::unordered_map` (caching):

#### Stage 1 Deduplication: `BloomFilter` vs. `std::unordered_set`

| Scale | `Sentrix BloomFilter` Time | `std::unordered_set` Time | Bloom Struct Mem | Peak RSS Delta (Bloom vs. Set) | Empirical FPR |
|---:|---:|---:|---:|---|---:|
| **10,000** | **0.33 ms** | 0.67 ms | 9.37 KiB | `+60.00 KiB` vs. `+592.00 KiB` | 0.977% |
| **100,000** | **2.88 ms** | 8.35 ms | 93.68 KiB | `+140.00 KiB` vs. `+5.62 MiB` | 1.009% |
| **1,000,000** | **46.49 ms** | 531.69 ms (**11.4x slower**) | **936.81 KiB** | **`+992.00 KiB` vs. `+55.46 MiB` (~57x less)** | **0.979%** |

#### Stage 2 Bounded Caching & Pipeline vs. Unbounded `std::unordered_map` (1,000,000 Payloads)

| Structure | Duplicate Hits | Duplicate Hit Rate | Evictions | Struct Memory | Peak RSS (+Delta) |
|---|---:|---:|---:|---:|---:|
| **`Sentrix LRUCache` (cap 50K)** | 169,691 | 84.85% | 780,309 | **7.61 MiB** | **46.29 MiB (`+9.72 MiB`)** |
| **`Sentrix Pipeline` (Bloom + LRU)** | 169,691 | 84.85% | 780,309 | **8.52 MiB** | **47.25 MiB (`+10.69 MiB`)** |
| **`std::unordered_map` (unbounded)** | 200,000 | 100.00% | 0 | 91.02 MiB | 153.29 MiB (`+116.73 MiB`) |

At **1M payloads**, `SentrixPipeline` bounds memory at **`+10.69 MiB` RSS delta** compared to **`+116.73 MiB`** for an unbounded `std::unordered_map` (**10.9x lower memory footprint**), while the Stage 1 Bloom gate **skips `798,662` unnecessary hash-map lookups** on unseen keys.

---

## Build, Test & Run

Requirements: **CMake 3.16+** and a **C++17** compiler (GCC 8+, Clang 7+, or MSVC 2019+).

```bash
git clone https://github.com/ravitkataria891-vortex/Sentrix.git
cd Sentrix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run the 80-check unit & integration test suite (active in both `Debug` and `Release` builds):

```bash
./build/sentrix_tests
```

Run the multi-mode & Stage 0 extensibility demo (or pass `--interactive` for the live CLI REPL):

```bash
./build/sentrix
./build/sentrix --interactive
```

Run the 10K / 100K / 1M STL baseline & real-world cost benchmark suite:

```bash
./build/sentrix_benchmark
./build/sentrix_benchmark 1000000 --repeats 5   # Custom scale / repeat count
./build/sentrix_benchmark --no-memory           # Skip subprocess RSS probes
```

---

## Project Layout

```text
Sentrix/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── src/
│   ├── hash_utils.hpp      # 32-bit FNV-1a & MurmurHash3 declarations
│   ├── hash_utils.cpp      # Canonical FNV-1a & MurmurHash3 implementations
│   ├── bloom_filter.hpp    # Dynamic std::vector<bool> BloomFilter + computed flush bound
│   ├── lru_cache.hpp       # O(1) Hash Map + unique_ptr Doubly Linked List LRU Cache
│   ├── pipeline.hpp        # Generic Stage chain, BloomStage, CacheStage & SentrixPipeline
│   ├── memory_usage.hpp    # Cross-platform Peak RSS & isolated subprocess measurement
│   ├── main.cpp            # Prompt caching, event dedup, Stage 0 hook & interactive REPL
│   └── benchmark.cpp       # 10K/100K/1M STL baselines + Help-Desk/Retry cost experiments
└── tests/
    └── test_sentrix.cpp    # Release-safe unit & integration test suite (80 checks)
```

---

## Team Responsibilities

| Team Member | Roll No. | Role | Algorithmic Responsibilities |
|---|---|---|---|
| **Ravit Kataria** | `B25EE1031` | Systems Architect | Generic stage-chain integration loop (`DROP` / `RESOLVED` / `CONTINUE`), LRU map-to-list pointer-ownership architecture, CMake build configuration, and multi-scale / use-case benchmark suite. |
| **Imdadul Hoque Laskar** | `B25EE1015` | Data Structures Lead | `O(1)` `LRUCache` synchronizing `std::unordered_map` with the `std::unique_ptr` doubly linked list, Peak RSS memory profiling, interactive CLI demo, and documentation. |
| **Jaidev Mahadev Araundekar** | `B25CY1008` | Algorithms Developer | `BloomFilter` over `std::vector<bool>`, 32-bit FNV-1a and MurmurHash3, mathematical flush-threshold saturation policy, and false-positive / unit test verification. |

---

## Extensibility & Future Scope

Because `Pipeline` processes payloads through an ordered, stage-agnostic `Stage` interface (`DROP`, `RESOLVED`, `CONTINUE`), new stages can be inserted or removed without modifying `BloomFilter` or `LRUCache` internals (`src/main.cpp` already demonstrates plugging in a custom `Stage 0` normalizer at runtime):

1. **Stage 0 Query-Normalization Trie:** Introducing a prefix Trie ahead of `BloomStage` to strip filler words and canonicalize near-duplicate phrasings (e.g., collapsing *"What is X?"* and *"Tell me about X"* into a single key), directly increasing cache-hit rates on natural-language AI traffic without complex failure-link state.
2. **Payload Pattern Matching (Aho–Corasick Automaton):** Reintroducing the original three-stage design's Aho–Corasick multi-pattern scanner as an independent filtering stage for workloads requiring threat-signature, PII, or blocked-keyword detection at line rate.
3. **Thread Safety & Concurrent Ingestion:** Extending the single-threaded core with striped locking or lock-free atomic bit operations on the Bloom Filter and sharded LRU cache segments to support multi-threaded ingestion without global lock contention.
4. **Adaptive / Counting Bloom Filter:** Replacing the full-reset saturation policy with a counting or decaying Bloom Filter so individual keys can expire or be deleted incrementally without incurring cold-start resets after saturation.
5. **HTTP / gRPC Service Wrapper:** Wrapping `SentrixPipeline` (building on the existing `--interactive` CLI REPL) in a lightweight HTTP or gRPC proxy to expose live hit-rate, false-positive, and cost-savings telemetry to external services.
6. **Snapshot Persistence:** Periodically snapshotting the Bloom Filter bit vector and hot LRU entries to disk so the engine can warm-start across restarts without re-learning frequent queries from scratch.
