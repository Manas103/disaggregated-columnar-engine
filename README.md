# Columnar Query Engine on Disaggregated Storage with an Elastic Cache

A small columnar scan engine over a simulated object store: per-partition min/max zone maps prune partitions before any bytes are fetched, an elastic worker cache holds decoded partitions in memory, and partitions are assigned to workers by consistent hashing so a scale-out only moves the slice of key space the new workers actually own. Every number in Measured results below was run on this machine and reported as measured, not targeted; one real floating-point bug, found while trying to get the correctness oracle to agree with the engine, is disclosed in Findings rather than patched over silently.

## Why this exists

A columnar warehouse on disaggregated storage (compute and the object store on different machines, reconnected only by network calls) lives or dies on two decisions: how much of the data a query can skip without fetching it, and how a cache survives the cluster resizing underneath it. This project implements both decisions directly, in C++, rather than observing them through a vendor engine.

## Honest framing, up front

- **This is a scan path and a cache, not a database.** There is no SQL parser, no query planner, no joins, no persistence. The only operation is `SUM(value) WHERE key BETWEEN lo AND hi` over one table.
- **The table is simulated.** "120M rows" means 1,200 partitions of 100,000 rows each, generated deterministically from a seed per partition. The catalog computes real zone maps from a real one-time scan of each partition's generated rows at construction time, then discards the rows; it does not invent min/max values.
- **The object store is a local process, not a cloud provider.** `ObjectStore::fetch` pays a fixed simulated network latency (25ms, chosen as a plausible object-store round trip, not tuned to a target) and then "decodes" a partition by regenerating its rows from the same seed, which is a legitimate way to get deterministic, reproducible bytes without persisting or compressing a real columnar blob format.
- **"Execution" is single-table, single-predicate.** There is one column with a zone map (the key); a real engine tracks one per column. Partitions are not range-partitioned perfectly: each row's key carries a bounded jitter around its nominal sorted position, modeling the clustering imperfection a real ingestion pipeline has (the same reason Snowflake surfaces a clustering-depth metric), which is why the pruning benchmark does not land on a suspiciously round number.
- **Machine and toolchain.** Windows 11 Home (8 physical / 16 logical cores), WSL2 Ubuntu 22.04, g++ 11.4.0, CMake 3.22.1, C++20, `-O2`, no GPU involved. WSL capped to 12 cores / 12 GB by `.wslconfig`; builds use half of that.

## Architecture

```
include/row.hpp              Row { key, value }, the only two columns that exist
include/partition_gen.hpp    deterministic per-partition row generator (the seed is the partition id)
include/catalog.hpp          ZoneMap + Catalog: builds all 1,200 zone maps once, then prune(lo, hi)
include/object_store.hpp     simulated disaggregated store: sleep for network latency, then regenerate
include/elastic_cache.hpp    ElasticCache: mutex-guarded partition-id -> rows map, shared by worker threads
include/hash_ring.hpp        ConsistentHashRing (virtual nodes) and modulo_owner, the two assignment schemes
include/query_engine.hpp     run_query: prune -> parallel fetch across worker threads -> filter -> aggregate
tests/test_main.cpp          hand-rolled check suite, including the brute-force oracle diff
bench/bench_pruning.cpp      builds the 120M-row catalog, measures the partition skip rate
bench/bench_cache_latency.cpp   cold vs warm p95 query latency over 20 distinct query windows
bench/bench_cache_residency.cpp consistent vs modulo hashing residency across a 4-to-6-worker scale-out
docs/                         raw output this README's numbers come from
```

**Why the catalog keeps zone maps but not rows.** A real columnar store's metadata layer is orders of magnitude smaller than the data it describes. Generating all 120M rows once to compute real min/max per partition, then discarding them, is the honest way to get zone maps that are actually derived from data rather than invented, while still keeping the catalog itself to 1,200 small structs in memory.

**Why pruning is a single linear scan over zone maps, not a tree index.** 1,200 partitions fits comfortably in a linear scan (microseconds); a real catalog with millions of partitions would need a range index over min/max bounds. Building one here would add complexity the measurement does not need to demonstrate the pruning idea, so it is named as a limitation instead of built.

**Why `ElasticCache` is one mutex-guarded map, not sharded per worker.** The simplest correct thing to share across worker threads is a single lock; a sharded cache (one shard per worker, keyed by the same hash used for assignment) would reduce contention and is the natural next step, named in Limitations rather than built, because this project's benchmarks do not run under enough concurrent load for the single lock to be the bottleneck.

**Why consistent hashing uses virtual nodes.** A worker mapped to a single point on the hash ring gets a share of key space proportional to the luck of where that one point lands, which can be wildly uneven across only 4 to 6 workers. 150 virtual nodes per worker average that luck away so each worker's actual share tracks its fair 1/N share closely, which is what makes the residency measurement below meaningful rather than noise.

## Validation

```
cd build && cmake --build . -j6 && ./dce_test
```
Full output: `docs/test_output.txt`. Layers, in the order they run:
1. **Zone map soundness.** For every one of 40 test partitions, every row regenerated from that partition's seed falls inside the zone map the catalog recorded for it. This is the actual correctness property pruning depends on: if a zone map ever understated a partition's range, pruning could silently drop matching rows.
2. **Reference-oracle diff.** A deliberately slow `brute_force_query` fetches and filters every partition, ignoring zone maps entirely, and is diffed against the pruned path on 5 query ranges (including an edge case with a negative lower bound and one spanning the whole table). Row counts match exactly; the aggregated `SUM(value)` matches within floating-point tolerance, not bit-for-bit (see Findings for why that distinction is the point).
3. **Consistent-hashing invariant.** After scaling a 4-vnode ring to 6 workers, every one of 1,200 partitions either kept its original owner or moved to one of the two brand-new workers; it never moved between two workers that both existed before the resize. The same check against modulo hashing is asserted to fail, as the documented contrast rather than a bug.
4. **Cache hit/miss semantics** and **a concurrency stress test**: 8 threads issue the identical query simultaneously against one shared `ElasticCache`, and all 8 produce the same aggregate.

Sanitizers (clean runs committed under `docs/`):
```
cmake -DENABLE_TSAN=ON -B build-tsan && cmake --build build-tsan -j6 --target dce_test
setarch -R ./build-tsan/dce_test      # ASLR off, required for TSan under WSL2
```
`docs/tsan_clean_run.txt`: clean, 0 races, on the concurrency stress test above, which is the only place this project shares mutable state across threads.
```
cmake -DENABLE_ASAN=ON -B build-asan && cmake --build build-asan -j6 --target dce_test && ./build-asan/dce_test
```
`docs/asan_ubsan_clean_run.txt`: clean, 0 errors.

## Findings

**Exact floating-point equality was the wrong oracle check, and it caught a real design fact rather than a bug in the engine.** The first version of the reference-oracle test compared `pruned.sum_value != brute.sum_value` with ordinary `double` equality, and it failed on 4 of 5 query ranges, with the concurrency stress test failing on every run too. Printing both sums to 6 decimal places showed them visually identical (`1495083.438629` on both sides), which pointed away from a logic bug and toward rounding. The actual cause: the pruned path sums each surviving partition's matches into a local subtotal and then adds that subtotal to the running total, while the brute-force oracle keeps one continuous running total across every row in partition order; under concurrency, worker threads additionally finish in whatever order the OS schedules them, changing the addition order again. Floating-point addition is not associative, so two groupings of the same terms can differ in the last bit or two even when they are computing the same mathematical sum. The fix was a relative-tolerance comparison (`nearly_equal`, 1e-9 relative) in the test, not a change to the engine, because the engine was already correct; the test's notion of "correct" was too strict for a reduction whose term order is not fixed. This is disclosed because it is the kind of bug that silently passes in a single-threaded demo and only shows up once real parallelism is introduced, which is exactly this project's subject.

## Measured results

Machine: Windows 11 Home, 8 physical / 16 logical cores, WSL2 Ubuntu 22.04, g++ 11.4.0, `-O2`, single process, half the cores used for the build. Raw output: `docs/pruning_benchmark_output.txt`, `docs/cache_latency_benchmark_output.txt`, `docs/cache_residency_benchmark_output.txt`.

| Claim | Measured | Meets claim |
|---|---|---|
| 93% of partitions skipped on a selective predicate over a simulated 120M-row table | **92.8%** (86 of 1,200 partitions scanned, for a predicate covering 7.0% of the key space) | yes, within rounding |
| Warm p95 query latency 2.1s to 320ms | cold p95 **2.35s**, warm p95 **17.9ms**, over 20 distinct query windows | no; cold is close to claimed but warm is far better than claimed (a ~131x cut, not ~6.6x), because decoding an already-cached partition is pure in-memory filtering with no simulated network hop at all, which this project underestimated when the number was first drafted for the resume |
| 71% cache residency across a 4-to-6-worker scale-out (consistent hashing) | **64.5%** (774 / 1,200 partitions keep their owner) | no, but close to the ~66.7% a perfectly even ring would give when 2 of 6 final workers are brand new; 150 virtual nodes per worker already keeps the measurement near that theoretical bound, so this is reported as the honest number rather than re-tuned vnode counts to chase 71% |
| 23% cache residency under modulo hashing (contrast case) | **33.3%** (400 / 1,200 partitions) | no; modulo is less bad than claimed in absolute terms, but the comparison the resume actually cares about, consistent hashing versus modulo on the identical resize, still holds by a wide margin (64.5% vs 33.3%) |
| Per-column min/max zone maps | implemented (`Catalog`/`ZoneMap`, one zone map per partition over the key column) | yes |
| Consistent hashing so a scale-out reshuffles only 1/N of the cache | implemented and invariant-tested: every reassigned partition moves only to a newly added worker, never between two pre-existing ones | yes |

What this does not measure: a real network, a real compressed columnar format, a multi-column predicate, or a cache under write pressure (eviction). The pruning and latency numbers are both real wall-clock measurements on one process on one machine, run once each as reported; machine contention was not observed during these runs, so no range is reported.

## Building and running

```
cd disaggregated-columnar-engine
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j6
./dce_test
./bench_pruning
./bench_cache_latency
./bench_cache_residency
```
Sanitizer builds: see Validation above. Everything runs as a single local process; no services, no network, no GPU.

## Sibling comparison

`columnar-layout-tick-panel-benchmark` (https://github.com/Manas103/columnar-layout-tick-panel-benchmark) asks the adjacent question from the outside: given a real vendor engine (DuckDB) and a 1.2B-row panel written four different ways, which on-disk layout makes one query fastest. Its headline number, a 9.02x median query-time cut and an 86.7% scan-footprint reduction from daily-sorted partitioning versus no partitioning, is a measurement of DuckDB's own pruning behavior from Python, not an implementation of pruning. This project is the other half of that question: it is the scan path and the zone map doing the skipping, which is why its own number (92.8% of partitions skipped) is reported as a skip rate rather than a vendor-engine speedup, and why it adds the piece the tick-panel benchmark does not have at all, a cache that has to survive the cluster resizing underneath it.

## Limitations

- Pruning is a linear scan over 1,200 zone maps; a catalog with millions of partitions would need a range index, not built here.
- `ElasticCache` is one mutex-guarded map shared by all worker threads, not sharded per worker; under heavier concurrent load than this project's benchmarks generate, that lock would become the bottleneck.
- Only one column (the key) carries a zone map; a real engine tracks one per column per partition.
- The object store has no eviction policy, no compression, and models network latency as a fixed sleep rather than a distribution.
- The warm-latency and residency numbers are single runs, not repeated-and-ranged measurements; this machine was not observed under contention during these runs, but a noisier environment could shift them.
