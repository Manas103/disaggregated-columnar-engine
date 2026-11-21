#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "partition_gen.hpp"
#include "row.hpp"

namespace dce {

// A simulated disaggregated object store. A real one is a network hop away
// (S3, Azure Blob, a cloud provider's object storage); this one is a single
// process standing in for that hop. "Fetching" a partition pays a fixed
// simulated network latency (fetch_latency) and then decodes the blob by
// deterministic regeneration, which is honest about what it does not model:
// no real network, no real compressed columnar blob format, no disk at all.
class ObjectStore {
public:
    ObjectStore(uint32_t rows_per_partition, int64_t jitter,
                std::chrono::microseconds fetch_latency)
        : rows_per_partition_(rows_per_partition), jitter_(jitter), fetch_latency_(fetch_latency) {}

    std::vector<Row> fetch(uint32_t partition_id) {
        fetch_count_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(fetch_latency_);
        return generate_partition_rows(partition_id, rows_per_partition_, jitter_);
    }

    uint64_t fetch_count() const { return fetch_count_.load(std::memory_order_relaxed); }

private:
    uint32_t rows_per_partition_;
    int64_t jitter_;
    std::chrono::microseconds fetch_latency_;
    std::atomic<uint64_t> fetch_count_{0};
};

}  // namespace dce
