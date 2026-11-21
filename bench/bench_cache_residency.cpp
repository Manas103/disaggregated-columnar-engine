// Measures cache residency across a worker scale-out: of the partitions
// assigned to a 4-worker cluster, what fraction keep the same owning
// worker (and therefore keep their cached, decoded data valid) after the
// cluster grows to 6 workers. Compares consistent hashing against modulo
// hashing on the identical partition set. Run with: ./bench_cache_residency
#include <cstdio>
#include <vector>

#include "hash_ring.hpp"

int main() {
    const uint32_t kPartitions = 1200;
    const uint32_t kVnodesPerWorker = 150;

    // Consistent hashing
    dce::ConsistentHashRing ring(kVnodesPerWorker);
    for (uint32_t w = 0; w < 4; ++w) ring.add_worker(w);
    std::vector<uint32_t> ring_before(kPartitions);
    for (uint32_t pid = 0; pid < kPartitions; ++pid) ring_before[pid] = ring.owner(pid);
    ring.add_worker(4);
    ring.add_worker(5);
    uint32_t ring_resident = 0;
    for (uint32_t pid = 0; pid < kPartitions; ++pid) {
        if (ring.owner(pid) == ring_before[pid]) ring_resident++;
    }

    // Modulo hashing, same partitions, same 4 -> 6 resize.
    uint32_t modulo_resident = 0;
    for (uint32_t pid = 0; pid < kPartitions; ++pid) {
        if (dce::modulo_owner(pid, 4) == dce::modulo_owner(pid, 6)) modulo_resident++;
    }

    double ring_pct = 100.0 * ring_resident / kPartitions;
    double modulo_pct = 100.0 * modulo_resident / kPartitions;

    std::printf("partitions: %u, scale-out: 4 workers -> 6 workers\n", kPartitions);
    std::printf("consistent hashing: %u / %u partitions kept their owner\n", ring_resident, kPartitions);
    std::printf("modulo hashing:     %u / %u partitions kept their owner\n", modulo_resident, kPartitions);
    std::printf("CONSISTENT_RESIDENCY_PCT: %.1f\n", ring_pct);
    std::printf("MODULO_RESIDENCY_PCT: %.1f\n", modulo_pct);
    return 0;
}
