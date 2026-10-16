# Ultra-Low-Latency In-Memory Matching Engine & Orderbook

A high-performance, single-file limit order book and matching engine written in modern C++23. Designed from scratch for deterministic, sub-100ns execution latencies targeting high-frequency trading (HFT) infrastructure.

### AI assistance disclosure

Only this readme file and benchmarking code is AI generated - the core orderbook logic + implementation is 100% mine.

---

## Architecture & Core Design Decisions

1. **Pre-Allocated Order Arena (`BOOK_BUFFER`)**
    - Eliminates all dynamic memory allocation (`malloc`/`new`) during runtime by statically pre-allocating an arena of 1,000,001 order objects at startup.

2. **Hierarchical Bitmaps & $O(1)$ Lookups**
    - Replaces traditional sorted containers or complex tree structures with 128-bit bitmaps and hierarchical summary tables (`master_bid_table`, `bids_summary`, `bids_bitmap`).
    - Leverages compiler intrinsics (`__builtin_ctzll` and `__builtin_clzll`) to find the best bid and best ask in constant time ($O(1)$) without traversing entire book levels.

3. **Intrusive Doubly-Linked Lists**
    - Price levels maintain intrusive DLLs directly within the order structs (`prev` and `next` indices), enabling instant order append, cancellation, and modification without pointer chasing or heap overhead.

4. **Hardware & Silicon Tuning (Apple Silicon M-Series)**
    - Integrates Mach absolute time (`mach_absolute_time`) for ultra-precise nanosecond benchmarking.
    - Tunes thread execution via `pthread_set_qos_class_self_np` to force scheduling onto high-performance CPU cores.
    - Cache-line aligns order structs (`alignas(CacheLineSize)`) to prevent false sharing.

---

## Supported Operations & Interface

- **Limit Orders (`buy` / `sell`)**: Add resting liquidity or execute matching against the book.
- **Market Orders**: Omit the price parameter to aggressively sweep the opposite side of the book.
- **Order Modification (`modify`)**: Update quantity, price, or side with intelligent priority-loss handling (re-queues orders if priority is lost).
- **Order Cancellation (`cancel`)**: O(1) unlinking and bitmap state toggling.
- **Top-of-Book Queries (`gtl <levels>`)**: Extract aggregate volume and price levels using hierarchical bitmap walks.

---

## Performance & Benchmarks

Measured under benchmark mode using macOS Mach timers (`mach_absolute_time`) across thousands of order transactions:

- **p50 Latency:** Achieves rock-solid **40 ns to 100 ns per command** hot-path execution.
- **Deterministic Memory:** Zero page faults and zero heap churn during matching loops.

```zsh
#./benchmark/commands.txt was generated through a python script - not relevant for the repo as such
./orderbook --benchmark-capacity 11000 < ./benchmark/commands.txt
Benchmark commands: 9497
Total command time: 904446 ns
Mean latency: 95 ns/command
p50 latency: 41 ns
p90 latency: 83 ns
p99 latency: 1125 ns
```