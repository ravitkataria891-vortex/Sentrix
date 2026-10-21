# Sentrix

**A high-performance C++ engine for eliminating redundant computation at scale.**

Sentrix detects repeated requests and instantly serves the answer it already knows, cutting redundant compute and API cost in high-traffic and AI-driven systems.

![C++17/20](https://img.shields.io/badge/C%2B%2B-17%2F20-blue)
![Status](https://img.shields.io/badge/status-prototype-orange)

---

## Overview

Most systems waste work repeating requests they have already seen and recomputing answers they already know.

Sentrix is a lightweight, single-threaded C++ pipeline that answers two questions in near-constant time, before any expensive work happens downstream:

1. **"Have I seen this before?"** answered by a probabilistic **Bloom Filter** in `O(k)`.
2. **"Do I already know the answer?"** answered by an **LRU Cache** (hash map + doubly linked list) in `O(1)`.

The project was deliberately scoped down from an earlier three-stage design that included a Trie / Aho-Corasick stage. That stage is kept as a documented extension point (see [Future Work](#future-work)), in favor of a correct, benchmarked, defensible prototype.

## Use Cases

| Use case | How Sentrix helps |
|---|---|
| **AI prompt / query caching** (primary) | The Bloom Filter flags prompts that have been seen before, and the LRU Cache serves the stored response instead of re-invoking an expensive model call. |
| **API request deduplication** | Suppresses duplicates caused by client retries or double-submitted forms and serves cached responses for recently repeated calls. A lightweight take on idempotency-key handling. |
| **Duplicate event / log ingestion filtering** | In at-least-once delivery systems, re-delivered events are dropped, and the cache returns the previous processing result. |
| **Market tick deduplication** | Drops duplicate ticks from redundant feeds and keeps the most recent price per symbol available at `O(1)` lookup cost. |

## Architecture

Incoming payloads pass through an ordered chain of stages. Each stage returns one of `DROP`, `RESOLVED`, or `CONTINUE`, and the loop halts at the first non-`CONTINUE` result.

```
                 incoming payload
                        |
                        v
        +-------------------------------+
        |  Stage 1: Bloom Filter        |   O(k)
        |  "have I seen this before?"   |
        +---------------+---------------+
                        |
        possibly seen   |   definitely new
        (check cache)   |   (pass through)
                        v
        +-------------------------------+
        |  Stage 2: LRU Cache           |   O(1)
        |  "do I already know the       |
        |   answer?"                    |
        +---------------+---------------+
                        |
          hit: RESOLVED | miss: CONTINUE
                        v
               expensive downstream work
```

The stage interface is intentionally stage-agnostic, so new stages can be inserted or removed without modifying the Bloom Filter or LRU Cache internals.

### Stage 1: Duplicate Filter (Bloom Filter)

A space-efficient, multi-hash bit vector. It is the cheapest check, so it runs first to drop redundant work early.

### Stage 2: Fast-Path Cache (LRU Cache)

A hash map combined with a doubly linked list. Cached results are served in `O(1)`, and the least-recently-used entry is evicted when the cache is at capacity.

## Technical Specifications

| Domain | Implementation strategy |
|---|---|
| **Language** | C++17/20, chosen for deterministic memory control and low-level pointer manipulation. |
| **Memory management** | `std::unique_ptr` owns the LRU nodes in the linked list. The hash map holds non-owning raw pointers for `O(1)` lookup, which avoids reference cycles. |
| **Data structures** | Bloom Filter over a runtime-sized `std::vector<bool>` (replacing `std::bitset` to allow dynamic sizing across benchmark runs). Hash map + custom doubly linked list for the LRU Cache. |
| **Hashing** | FNV-1a and MurmurHash. |
| **Saturation handling** | A capacity flush threshold resets the Bloom Filter once the insert count crosses a computed bound, so permanent saturation (a 100% false-positive state) cannot silently block all traffic. |
| **Concurrency** | Single-threaded in this version. This keeps correctness verifiable within the available build time. Thread safety is tracked as future work. |
| **Tooling** | Google Antigravity AI IDE was used strictly as a boilerplate generator for isolated, self-contained components (e.g. hash functions). All cross-structure ownership and integration code is hand-written and validated to avoid AI-introduced pointer or lifetime bugs. |

## Build and Run

> **Note:** adjust the file names below to match your actual repo layout.

**Requirements:** a C++17-capable compiler (GCC 9+, Clang 10+, or MSVC 2019+).

```bash
git clone https://github.com/<your-username>/sentrix.git
cd sentrix

# Build (example; replace with your actual sources or CMake setup)
g++ -std=c++17 -O2 -Wall -Wextra -o sentrix src/*.cpp

# Run
./sentrix
```

## Benchmarks

A dedicated script generates a reproducible stream of **1 million payloads (80% unique / 20% duplicate)** and measures execution time at **10K / 100K / 1M** payload counts against named standard-library baselines:

- `std::unordered_set` for deduplication
- a plain `std::unordered_map` with no eviction for caching

It also reports the Bloom Filter's **false-positive rate** at the chosen bit-array size and hash-function count, and **peak memory footprint (RSS)** against both baselines.

### Results

> Benchmarks in progress. Numbers will be added here once the benchmark script has been run.

| Payloads | Sentrix (ms) | `unordered_set` (ms) | `unordered_map` (ms) |
|---|---|---|---|
| 10K | TBD | TBD | TBD |
| 100K | TBD | TBD | TBD |
| 1M | TBD | TBD | TBD |

| Metric | Sentrix | Baseline |
|---|---|---|
| Bloom Filter false-positive rate | TBD | n/a |
| Peak memory (RSS) | TBD | TBD |

## Design Decisions

- **Two stages, not three.** The original design included a Trie / Aho-Corasick stage. It was cut to prioritize a correct, benchmarked prototype over an ambitious but unfinished one.
- **`std::vector<bool>` over `std::bitset`.** The bit-array size must be configurable at runtime so benchmarks can sweep across sizes.
- **Clear pointer ownership.** Linked-list nodes are uniquely owned; the hash map only observes them. This removes ownership ambiguity and reference cycles.
- **Flush-and-reset saturation policy.** Simple and predictable. Its cold-start cost is the motivation for the counting Bloom Filter extension below.
- **Single-threaded by design.** A scoping decision, not an oversight. See Future Work.

## Future Work

Because stages are an ordered, generic chain, each of these can be added independently:

- **Query-normalization Trie (Stage 0).** Runs before the Bloom Filter to strip filler words and normalize queries (e.g. "What is X?" and "Tell me about X" collapse toward a common key), raising the effective cache-hit rate.
- **Payload pattern matching (Aho-Corasick).** Multi-pattern scanning of payload content, such as threat-signature or keyword detection. Higher effort and higher risk than the normalization Trie.
- **Thread safety.** Fine-grained locking or a lock-free design for the Bloom Filter bit vector and the LRU hash map / linked-list pair, enabling concurrent ingestion.
- **Adaptive / counting Bloom Filter.** Lets individual entries expire or be removed independently, avoiding the full-reset cold start on saturation.
- **Demo and interface layer.** A small CLI or HTTP wrapper to submit payloads, observe cache hits and misses, and view live false-positive and hit-rate statistics.
- **Persistence.** Periodic snapshots of the Bloom Filter bit vector and LRU cache contents to disk so the pipeline can resume its learned state across restarts.

## Team

| Member | Role | Responsibilities |
|---|---|---|
| **Ravit Kataria** | Systems Architect | Integration loop and stage-chain interface; pointer-ownership model between the LRU hash map and linked list; benchmarking script. |
| **Imdadul Hoque Laskar** | Data Structures Lead | LRU Cache with a custom doubly linked list synchronized with the hash map for `O(1)` eviction and pointer safety; caching-vs-baseline benchmark case. |
| **Jaidev Mahadev Araundekar** | Algorithms Developer | Bloom Filter over `std::vector<bool>`, FNV-1a / MurmurHash, flush-threshold policy, and false-positive rate measurement. |
