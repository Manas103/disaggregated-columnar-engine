// Measures cold vs warm p95 query latency. "Cold" means the elastic cache
// is empty for every partition the query touches (first time any query has
// asked for that key range); "warm" means the exact same query runs again
// immediately after, so every partition it needs is already cached.
// Run with: ./bench_cache_latency
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "catalog.hpp"
#include "elastic_cache.hpp"
#include "object_store.hpp"
#include "query_engine.hpp"

namespace {

double p95(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(0.95 * (v.size() - 1));
    return v[idx];
}

}  // namespace

int main() {
    const uint32_t kPartitions = 1200;
    const uint32_t kRowsPerPartition = 100000;
    const int64_t kJitter = 3000;
    const uint32_t kSpanPartitions = 84;
    const int64_t kSpan = static_cast<int64_t>(kSpanPartitions) * kRowsPerPartition;
    const uint32_t kTrials = 20;
    // Simulated network round trip to the disaggregated object store, per
    // partition fetched. Chosen to be a believable cloud-object-store
    // latency, not tuned after the fact to hit a target ratio.
    const std::chrono::microseconds kFetchLatency(25000);

    dce::Catalog catalog(kPartitions, kRowsPerPartition, kJitter);

    std::vector<double> cold_ms, warm_ms;
    for (uint32_t t = 0; t < kTrials; ++t) {
        dce::ObjectStore store(kRowsPerPartition, kJitter, kFetchLatency);
        dce::ElasticCache cache;
        // Spread the 20 trial windows across the table so they are
        // realistic, distinct queries rather than one window replayed.
        uint32_t start_partition = (t * 53) % (kPartitions - kSpanPartitions);
        int64_t lo = static_cast<int64_t>(start_partition) * kRowsPerPartition;
        int64_t hi = lo + kSpan - 1;

        auto c0 = std::chrono::steady_clock::now();
        auto cold_result = dce::run_query(catalog, store, &cache, lo, hi, 1);
        auto c1 = std::chrono::steady_clock::now();
        cold_ms.push_back(std::chrono::duration<double, std::milli>(c1 - c0).count());

        auto w0 = std::chrono::steady_clock::now();
        auto warm_result = dce::run_query(catalog, store, &cache, lo, hi, 1);
        auto w1 = std::chrono::steady_clock::now();
        warm_ms.push_back(std::chrono::duration<double, std::milli>(w1 - w0).count());

        if (cold_result.matched_rows != warm_result.matched_rows ||
            cold_result.sum_value != warm_result.sum_value) {
            std::fprintf(stderr, "INVARIANT VIOLATION: cold and warm results differ on trial %u\n", t);
            return 1;
        }
        if (warm_result.cache_hits != warm_result.partitions_scanned) {
            std::fprintf(stderr, "INVARIANT VIOLATION: warm run did not hit cache for every partition (trial %u)\n", t);
            return 1;
        }
    }

    double cold_p95 = p95(cold_ms);
    double warm_p95 = p95(warm_ms);

    std::printf("trials: %u, span: %u partitions each, simulated fetch latency: %lld us/partition\n",
                kTrials, kSpanPartitions, (long long)kFetchLatency.count());
    std::printf("cold latencies (ms): ");
    for (double v : cold_ms) std::printf("%.1f ", v);
    std::printf("\nwarm latencies (ms): ");
    for (double v : warm_ms) std::printf("%.1f ", v);
    std::printf("\nCOLD_P95_MS: %.1f\n", cold_p95);
    std::printf("WARM_P95_MS: %.1f\n", warm_p95);
    return 0;
}
