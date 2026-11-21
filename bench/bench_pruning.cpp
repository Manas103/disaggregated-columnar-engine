// Measures the pruning benchmark: how many of a simulated 120M-row table's
// partitions a selective predicate can skip, using only the catalog's zone
// maps. Run with: ./bench_pruning
#include <chrono>
#include <cstdio>

#include "catalog.hpp"

int main() {
    const uint32_t kPartitions = 1200;
    const uint32_t kRowsPerPartition = 100000;  // 1200 * 100000 = 120,000,000 rows (simulated)
    const int64_t kJitter = 3000;                // 3% of a partition's nominal span

    auto t0 = std::chrono::steady_clock::now();
    dce::Catalog catalog(kPartitions, kRowsPerPartition, kJitter);
    auto t1 = std::chrono::steady_clock::now();
    double build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // A query selecting 84 of 1200 partitions' nominal span (7.0% of the
    // key space), positioned in the middle of the table so both neighbors
    // are real partitions.
    const uint32_t kSpanPartitions = 84;
    const int64_t kSpan = static_cast<int64_t>(kSpanPartitions) * kRowsPerPartition;
    const int64_t kLo = 558LL * kRowsPerPartition;
    const int64_t kHi = kLo + kSpan - 1;

    auto survivors = catalog.prune(kLo, kHi);
    double prune_rate = 100.0 * (1.0 - static_cast<double>(survivors.size()) / kPartitions);

    std::printf("catalog build: %u partitions, %llu total rows (simulated), %.1f ms\n", kPartitions,
                static_cast<unsigned long long>(catalog.total_rows()), build_ms);
    std::printf("predicate: key BETWEEN %lld AND %lld (span = %u partitions' nominal width, %.2f%% of key space)\n",
                (long long)kLo, (long long)kHi, kSpanPartitions,
                100.0 * kSpan / catalog.total_rows());
    std::printf("partitions scanned: %zu / %u\n", survivors.size(), kPartitions);
    std::printf("PRUNE_RATE_PCT: %.1f\n", prune_rate);
    return 0;
}
