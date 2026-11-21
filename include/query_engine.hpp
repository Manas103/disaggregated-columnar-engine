#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "catalog.hpp"
#include "elastic_cache.hpp"
#include "object_store.hpp"
#include "row.hpp"

namespace dce {

struct QueryResult {
    uint32_t partitions_total = 0;
    uint32_t partitions_scanned = 0;
    uint64_t matched_rows = 0;
    double sum_value = 0.0;
    uint32_t cache_hits = 0;
};

// Scans every surviving (post-pruning) partition in parallel across
// num_workers threads, filters rows to [lo, hi] (a partition's zone map can
// overlap the predicate without every row in it matching, because of the
// jitter described in partition_gen.hpp), and aggregates SUM(value).
//
// If cache is non-null, a partition already present is read from it instead
// of fetched, and a freshly fetched partition is written back into it. This
// is the one function both the pruning benchmark and the cache-latency
// benchmark call, so "prune, then fetch, then filter" is exercised
// identically by both.
inline QueryResult run_query(const Catalog& catalog, ObjectStore& store, ElasticCache* cache,
                              int64_t lo, int64_t hi, uint32_t num_workers) {
    auto candidates = catalog.prune(lo, hi);

    QueryResult result;
    result.partitions_total = catalog.num_partitions();
    result.partitions_scanned = static_cast<uint32_t>(candidates.size());

    std::mutex agg_mu;
    std::atomic<size_t> next_index{0};

    auto worker_fn = [&]() {
        for (;;) {
            size_t i = next_index.fetch_add(1, std::memory_order_relaxed);
            if (i >= candidates.size()) break;
            uint32_t pid = candidates[i];

            bool hit = false;
            std::vector<Row> rows;
            if (cache != nullptr && cache->contains(pid)) {
                rows = cache->get(pid);
                hit = true;
            } else {
                rows = store.fetch(pid);
                if (cache != nullptr) cache->put(pid, rows);
            }

            uint64_t local_matched = 0;
            double local_sum = 0.0;
            for (const auto& r : rows) {
                if (r.key >= lo && r.key <= hi) {
                    local_matched++;
                    local_sum += r.value;
                }
            }

            std::lock_guard<std::mutex> lock(agg_mu);
            result.matched_rows += local_matched;
            result.sum_value += local_sum;
            if (hit) result.cache_hits++;
        }
    };

    uint32_t threads_to_use = std::max<uint32_t>(1, std::min(num_workers, static_cast<uint32_t>(candidates.size())));
    if (threads_to_use == 0) threads_to_use = 1;
    std::vector<std::thread> pool;
    pool.reserve(threads_to_use);
    for (uint32_t w = 0; w < threads_to_use; ++w) pool.emplace_back(worker_fn);
    for (auto& t : pool) t.join();

    return result;
}

}  // namespace dce
