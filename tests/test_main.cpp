// Hand-rolled check suite: this project needs a controlled, exact thread
// count for the concurrency test (the TSan run must be deterministic about
// how many threads touch the shared cache), which a framework's own runner
// does not guarantee, so a plain main() drives everything instead of
// GoogleTest.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "catalog.hpp"
#include "elastic_cache.hpp"
#include "hash_ring.hpp"
#include "object_store.hpp"
#include "partition_gen.hpp"
#include "query_engine.hpp"

namespace {

int g_failures = 0;

// SUM(value) is accumulated as one subtotal per partition in the pruned
// path (and, under concurrency, in whatever order threads finish), but as
// one continuous running total in the brute-force oracle. Both groupings
// are mathematically the same sum; floating-point addition is not
// associative, so they can differ in the last bit or two. A relative
// tolerance, not exact equality, is the correct check for a reduction
// whose term order is not fixed. See Findings in the README.
bool nearly_equal(double a, double b, double rel_tol = 1e-9) {
    double diff = std::fabs(a - b);
    double scale = std::max(std::fabs(a), std::fabs(b));
    return diff <= rel_tol * std::max(scale, 1.0);
}

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::fflush(stderr);
        g_failures++;
    } else {
        std::printf("ok: %s\n", what);
        std::fflush(stdout);
    }
}

// Scans every partition unconditionally, ignoring zone maps entirely. This
// is the deliberately slow, obviously-correct reference the pruned query is
// diffed against: if pruning ever skips a partition that actually holds a
// matching row, this function still finds it and the diff below catches it.
dce::QueryResult brute_force_query(const dce::Catalog& catalog, dce::ObjectStore& store, int64_t lo,
                                    int64_t hi) {
    dce::QueryResult result;
    result.partitions_total = catalog.num_partitions();
    result.partitions_scanned = catalog.num_partitions();
    for (uint32_t pid = 0; pid < catalog.num_partitions(); ++pid) {
        auto rows = store.fetch(pid);
        for (const auto& r : rows) {
            if (r.key >= lo && r.key <= hi) {
                result.matched_rows++;
                result.sum_value += r.value;
            }
        }
    }
    return result;
}

void test_zone_map_soundness() {
    const uint32_t kPartitions = 40;
    const uint32_t kRows = 2000;
    const int64_t kJitter = 60;
    dce::Catalog catalog(kPartitions, kRows, kJitter);
    bool all_sound = true;
    for (uint32_t pid = 0; pid < kPartitions; ++pid) {
        const auto& zm = catalog.zone_map(pid);
        if (zm.min_key > zm.max_key) {
            all_sound = false;
            break;
        }
        auto rows = dce::generate_partition_rows(pid, kRows, kJitter);
        for (const auto& r : rows) {
            if (r.key < zm.min_key || r.key > zm.max_key) {
                all_sound = false;
                break;
            }
        }
    }
    check(all_sound, "every zone map bounds every row it was built from");
}

void test_pruning_matches_brute_force_oracle() {
    // Deliberately small so the brute-force oracle (which fetches every
    // partition, including the ones pruning will skip) finishes quickly.
    const uint32_t kPartitions = 60;
    const uint32_t kRows = 1500;
    const int64_t kJitter = 50;
    dce::Catalog catalog(kPartitions, kRows, kJitter);
    dce::ObjectStore store(kRows, kJitter, std::chrono::microseconds(0));

    struct Range {
        int64_t lo, hi;
    };
    std::vector<Range> ranges = {
        {0, 3000},
        {20000, 35000},
        {50000, 90000},
        {-100, 500},
        {0, static_cast<int64_t>(kPartitions) * kRows},
    };

    bool all_match = true;
    bool saw_real_pruning = false;
    for (const auto& r : ranges) {
        auto pruned = dce::run_query(catalog, store, nullptr, r.lo, r.hi, 1);
        auto brute = brute_force_query(catalog, store, r.lo, r.hi);
        if (pruned.matched_rows != brute.matched_rows || !nearly_equal(pruned.sum_value, brute.sum_value)) {
            all_match = false;
            std::fprintf(stderr, "  diff on [%lld,%lld]: pruned matched=%llu sum=%.6f | brute matched=%llu sum=%.6f\n",
                         (long long)r.lo, (long long)r.hi, (unsigned long long)pruned.matched_rows,
                         pruned.sum_value, (unsigned long long)brute.matched_rows, brute.sum_value);
        }
        if (pruned.partitions_scanned < kPartitions) saw_real_pruning = true;
    }
    check(all_match, "pruned query result matches the brute-force oracle (exact row/match count, sum within float tolerance)");
    check(saw_real_pruning, "at least one query actually skipped partitions (pruning is not a no-op)");
}

void test_consistent_hash_minimal_disruption() {
    const uint32_t kPartitions = 1200;
    const uint32_t kVnodes = 150;
    dce::ConsistentHashRing ring(kVnodes);
    for (uint32_t w = 0; w < 4; ++w) ring.add_worker(w);

    std::vector<uint32_t> before(kPartitions);
    for (uint32_t pid = 0; pid < kPartitions; ++pid) before[pid] = ring.owner(pid);

    ring.add_worker(4);
    ring.add_worker(5);

    // The real invariant: a key either stays with its old owner, or moves
    // to one of the two brand-new workers. It must never move between two
    // workers that were both already on the ring before the resize.
    bool invariant_holds = true;
    for (uint32_t pid = 0; pid < kPartitions; ++pid) {
        uint32_t after = ring.owner(pid);
        if (after != before[pid] && after != 4 && after != 5) {
            invariant_holds = false;
            break;
        }
    }
    check(invariant_holds, "consistent hashing only moves keys to newly added workers, never between old ones");

    // The same invariant does not hold for modulo hashing, which is exactly
    // why the engine does not use it; this assertion documents the contrast
    // rather than treating modulo as a bug.
    bool modulo_violates = false;
    for (uint32_t pid = 0; pid < kPartitions; ++pid) {
        uint32_t before_m = dce::modulo_owner(pid, 4);
        uint32_t after_m = dce::modulo_owner(pid, 6);
        if (after_m != before_m && after_m != 4 && after_m != 5) {
            modulo_violates = true;
            break;
        }
    }
    check(modulo_violates, "modulo hashing (contrast case) does move keys between two pre-existing workers");
}

void test_elastic_cache_hit_miss_semantics() {
    dce::ElasticCache cache;
    check(!cache.contains(7), "cache starts empty");
    std::vector<dce::Row> rows = {{1, 1.0}, {2, 2.0}};
    cache.put(7, rows);
    check(cache.contains(7), "cache contains a partition after put");
    auto got = cache.get(7);
    check(got.size() == 2 && got[0].key == 1 && got[1].key == 2, "cache returns the exact rows it was given");
    check(!cache.contains(8), "an unrelated key is still a miss");
}

// Many threads issue the same query concurrently against one shared cache.
// This is what the TSan build (docs/tsan_clean_run.txt) actually exercises:
// without ElasticCache's mutex this reliably races on the underlying
// unordered_map.
void test_concurrent_queries_share_cache_safely() {
    const uint32_t kPartitions = 200;
    const uint32_t kRows = 500;
    const int64_t kJitter = 20;
    dce::Catalog catalog(kPartitions, kRows, kJitter);
    dce::ObjectStore store(kRows, kJitter, std::chrono::microseconds(100));
    dce::ElasticCache cache;

    std::vector<std::thread> threads;
    std::vector<dce::QueryResult> results(8);
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t]() {
            results[t] = dce::run_query(catalog, store, &cache, 0,
                                         static_cast<int64_t>(kPartitions) * kRows, 4);
        });
    }
    for (auto& th : threads) th.join();

    bool all_same = true;
    for (int t = 1; t < 8; ++t) {
        if (results[t].matched_rows != results[0].matched_rows ||
            !nearly_equal(results[t].sum_value, results[0].sum_value)) {
            all_same = false;
        }
    }
    check(all_same, "8 concurrent queries sharing one cache all compute the identical aggregate");
}

}  // namespace

int main() {
    test_zone_map_soundness();
    test_pruning_matches_brute_force_oracle();
    test_consistent_hash_minimal_disruption();
    test_elastic_cache_hit_miss_semantics();
    test_concurrent_queries_share_cache_safely();

    if (g_failures == 0) {
        std::printf("\nALL CHECKS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
