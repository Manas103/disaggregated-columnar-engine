#pragma once
#include <cstdint>
#include <random>
#include <vector>

#include "row.hpp"

namespace dce {

// Deterministic per-partition row generator. The same partition id always
// regenerates the exact same rows, which is what lets the object store
// "decode a blob" by regeneration instead of persisting one on disk, and
// lets the catalog recompute real zone maps instead of inventing them.
//
// Keys are not laid out in perfectly sorted partitions: each row's key is
// its nominal, sorted position plus a bounded jitter. That overlap between
// neighboring partitions' zone maps is intentional. It models imperfect
// clustering (the same reason Snowflake exposes a clustering-depth metric),
// and it is why the pruning benchmark does not land on a suspiciously exact
// number.
inline std::vector<Row> generate_partition_rows(uint32_t partition_id,
                                                 uint32_t rows_per_partition,
                                                 int64_t jitter,
                                                 uint64_t seed_base = 0x9E3779B97F4A7C15ULL) {
    std::vector<Row> rows;
    rows.reserve(rows_per_partition);
    std::mt19937_64 rng(seed_base + partition_id);
    std::uniform_int_distribution<int64_t> jitter_dist(-jitter, jitter);
    std::uniform_real_distribution<double> value_dist(0.0, 1000.0);

    const int64_t partition_base = static_cast<int64_t>(partition_id) * rows_per_partition;
    for (uint32_t i = 0; i < rows_per_partition; ++i) {
        int64_t nominal = partition_base + static_cast<int64_t>(i);
        int64_t key = nominal + jitter_dist(rng);
        rows.push_back(Row{key, value_dist(rng)});
    }
    return rows;
}

}  // namespace dce
