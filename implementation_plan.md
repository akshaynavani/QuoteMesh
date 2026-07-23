# QuoteMesh — Implementation Plan

**SIMD-Accelerated Multi-Exchange Market Data Aggregator**

Tech: C++20, CMake, `std::thread` + lock-free SPSC queues, SIMD (SSE2/AVX2 on x86-64, NEON on
ARM64, scalar fallback), POSIX shared memory (`shm_open` + `mmap`) for IPC, GoogleTest.

## Goal

A single process ingests real-time quote/trade feeds from 15+ exchanges (one feed handler
thread per exchange), parses the wire format with SIMD-accelerated routines, normalises the
messages, aggregates them into a consolidated per-symbol book (best bid/offer per venue and the
national best bid/offer — "NBBO"), and distributes updates to any number of downstream
processes through a lock-free shared-memory broadcast ring plus a seqlock-protected snapshot
table.

## Key design decisions

| Decision | Choice | Why |
|---|---|---|
| Wire format | Pipe-delimited ASCII lines (`Q\|seq\|sym\|bid\|bsz\|ask\|asz\|ts\n`) | Realistic text feed (FIX/ITCH-text style) where delimiter scanning & digit parsing dominate — exactly what SIMD speeds up |
| Prices | Fixed-point `int64` with 4 implied decimals | No floating point on the hot path; exact comparisons |
| SIMD | Delimiter bitmask scan (16/32 bytes per step) + SWAR/SIMD 8-digit integer parse | Portable across x86 and Apple Silicon; scalar reference path for correctness tests |
| Threading | N feed threads → SPSC rings → 1 aggregator thread → shm | Single writer per structure, no locks on the hot path |
| Feed sources | Pluggable: synthetic generator (in-process) and UDP (unicast/multicast) | Runnable without any exchange connectivity |
| IPC | Shared-memory broadcast ring (single producer, many readers, per-slot sequence numbers) + snapshot table with seqlocks | Readers never block the writer; slow readers detect overrun |
| Config | Simple `key=value` file + CLI flags, env var for shm name | No external dependencies |

## Phases

### Phase 1 — Core types, wire format, scalar parser
Deliverables:
- CMake project skeleton (C++20, `-O3 -march=native` option, tests via GoogleTest FetchContent).
- Domain types: `Price`, `Quote`, `Trade`, `MarketEvent`, `ExchangeId`, `SymbolId`.
- `SymbolTable` (string → dense id interning).
- Wire format encoder (used by generators) and reference scalar parser.
- Unit tests for encoder/parser round-trip.

Files: `CMakeLists.txt`, `include/quotemesh/{types,symbol_table,wire_format,parser}.hpp`,
`src/{symbol_table,wire_format,scalar_parser}.cpp`, `tests/CMakeLists.txt`, `tests/test_parser.cpp`.

### Phase 2 — SIMD parsing
Deliverables:
- `simd/delimiter_scan.hpp`: SSE2 / AVX2 / NEON / scalar implementations returning bitmasks of `|`
  and `\n` positions.
- `simd/number_parse.hpp`: SWAR 8-digit parse, fixed-point price parse.
- `SimdParser` that walks a buffer of many messages using the delimiter masks.
- Tests verifying SIMD == scalar on random fuzzed input; micro-benchmark.

Files: `include/quotemesh/simd/*.hpp`, `src/simd_parser.cpp`, `tests/test_simd.cpp`,
`bench/bench_parser.cpp`.

### Phase 3 — Multi-threaded ingestion & aggregation
Deliverables:
- Cache-line padded lock-free `SpscQueue<T>`.
- `FeedSource` interface with `SyntheticFeed` (per-exchange random walk generator) and `UdpFeed`.
- `FeedHandler` thread: read bytes → SIMD parse → sequence-gap detection → push to SPSC queue.
- `ConsolidatedBook`: per-symbol per-venue BBO + NBBO computation.
- `Aggregator` thread: drains all queues round-robin, updates books, emits NBBO changes.
- `Stats` counters (messages, gaps, parse errors, latency).

Files: `include/quotemesh/{spsc_queue,feed_source,synthetic_feed,udp_feed,feed_handler,consolidated_book,aggregator,stats}.hpp`, matching `src/*.cpp`, `tests/test_{spsc,book}.cpp`.

### Phase 4 — Shared-memory IPC distribution
Deliverables:
- `ShmRegion` RAII wrapper over `shm_open`/`mmap`.
- `BroadcastRing`: single-producer multi-consumer ring of fixed-size `ShmMessage` with per-slot
  sequence numbers; consumers detect overruns.
- `SnapshotTable`: seqlock-protected latest NBBO per symbol for late joiners.
- `ShmPublisher` (aggregator side) and `ShmSubscriber` (consumer side).
- Tests using an in-process region.

Files: `include/quotemesh/shm/{shm_region,layout,broadcast_ring,snapshot_table,publisher,subscriber}.hpp`,
`src/shm_region.cpp`, `tests/test_shm.cpp`.

### Phase 5 — Applications, config, benchmarks
Deliverables:
- `quotemesh` aggregator daemon (config file, 15+ exchanges by default, graceful shutdown, periodic stats).
- `qm_subscriber` CLI (tail NBBO stream or dump snapshot).
- `qm_feedsim` UDP feed simulator to drive the UDP path.
- End-to-end pipeline benchmark, sample config.

Files: `apps/*.cpp`, `include/quotemesh/config.hpp`, `src/config.cpp`, `config/quotemesh.conf`,
`bench/bench_pipeline.cpp`.

### Wrap-up
`.gitignore`, `README.md` with architecture diagram, run and test instructions.
