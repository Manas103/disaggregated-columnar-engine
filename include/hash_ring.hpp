#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dce {

inline uint64_t mix64(uint64_t x) {
    // SplitMix64 finalizer: a cheap, well-distributed integer hash, used so
    // vnode points and partition keys land on the ring independent of the
    // integers' own structure (two adjacent partition ids must not land
    // next to each other on the ring).
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

// Consistent hashing with virtual nodes. Adding a worker moves only the
// slice of key space newly owned by that worker's vnodes; it never moves a
// key between two workers that were already present. That invariant is
// checked directly in tests, not just approximated by measurement.
class ConsistentHashRing {
public:
    explicit ConsistentHashRing(uint32_t vnodes_per_worker) : vnodes_per_worker_(vnodes_per_worker) {}

    void add_worker(uint32_t worker_id) {
        for (uint32_t v = 0; v < vnodes_per_worker_; ++v) {
            uint64_t h = mix64((static_cast<uint64_t>(worker_id) << 32) ^ v ^ 0xA5A5A5A5ULL);
            ring_[h] = worker_id;
        }
        workers_.push_back(worker_id);
    }

    uint32_t owner(uint32_t partition_id) const {
        uint64_t h = mix64(partition_id);
        auto it = ring_.lower_bound(h);
        if (it == ring_.end()) it = ring_.begin();
        return it->second;
    }

    const std::vector<uint32_t>& workers() const { return workers_; }

private:
    uint32_t vnodes_per_worker_;
    std::map<uint64_t, uint32_t> ring_;
    std::vector<uint32_t> workers_;
};

// The naive alternative: owner = partition_id % num_workers. Resizing
// changes the modulus for every key, which is exactly why it is the
// contrast case for the residency benchmark.
inline uint32_t modulo_owner(uint32_t partition_id, uint32_t num_workers) {
    return partition_id % num_workers;
}

}  // namespace dce
