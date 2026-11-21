#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "row.hpp"

namespace dce {

// A per-process cache keyed by partition id. In the real system each worker
// has its own cache; here every worker thread shares one ElasticCache
// instance guarded by a single mutex, which is deliberately the simplest
// correct thing (a sharded cache would scale better and is noted as a
// limitation) and is exactly the shared state the TSan run exercises.
class ElasticCache {
public:
    bool contains(uint32_t partition_id) {
        std::lock_guard<std::mutex> lock(mu_);
        return cache_.find(partition_id) != cache_.end();
    }

    // Returns a copy so callers never hold a reference into the map while
    // another thread might rehash it.
    std::vector<Row> get(uint32_t partition_id) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = cache_.find(partition_id);
        if (it == cache_.end()) return {};
        hits_++;
        return it->second;
    }

    void put(uint32_t partition_id, std::vector<Row> rows) {
        std::lock_guard<std::mutex> lock(mu_);
        cache_[partition_id] = std::move(rows);
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return cache_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        cache_.clear();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<uint32_t, std::vector<Row>> cache_;
    uint64_t hits_ = 0;
};

}  // namespace dce
