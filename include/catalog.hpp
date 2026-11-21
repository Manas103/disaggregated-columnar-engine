#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "partition_gen.hpp"
#include "row.hpp"

namespace dce {

// Per-column min/max zone map for one partition. Only the key column is
// tracked; a real engine would carry one of these per column per partition.
struct ZoneMap {
    uint32_t partition_id;
    int64_t min_key;
    int64_t max_key;
    uint32_t row_count;
};

// The catalog is metadata only: it holds one ZoneMap per partition, built by
// a one-time real scan of that partition's generated rows at construction
// time. It never retains the rows themselves, which is what makes the table
// "simulated": the catalog represents the partition layout and statistics a
// real 120M-row table would have, without 120M rows living in memory or on
// disk at once.
class Catalog {
public:
    Catalog(uint32_t num_partitions, uint32_t rows_per_partition, int64_t jitter)
        : num_partitions_(num_partitions),
          rows_per_partition_(rows_per_partition),
          jitter_(jitter) {
        zone_maps_.reserve(num_partitions);
        for (uint32_t id = 0; id < num_partitions; ++id) {
            auto rows = generate_partition_rows(id, rows_per_partition, jitter);
            int64_t lo = std::numeric_limits<int64_t>::max();
            int64_t hi = std::numeric_limits<int64_t>::min();
            for (const auto& r : rows) {
                lo = std::min(lo, r.key);
                hi = std::max(hi, r.key);
            }
            zone_maps_.push_back(ZoneMap{id, lo, hi, static_cast<uint32_t>(rows.size())});
        }
    }

    uint32_t num_partitions() const { return num_partitions_; }
    uint32_t rows_per_partition() const { return rows_per_partition_; }
    int64_t jitter() const { return jitter_; }
    uint64_t total_rows() const {
        return static_cast<uint64_t>(num_partitions_) * rows_per_partition_;
    }
    const ZoneMap& zone_map(uint32_t partition_id) const { return zone_maps_.at(partition_id); }
    const std::vector<ZoneMap>& zone_maps() const { return zone_maps_; }

    // Returns the partition ids whose zone map intersects [lo, hi]. This is
    // the only place pruning happens: a partition is skipped if and only if
    // its recorded range cannot contain a matching row.
    std::vector<uint32_t> prune(int64_t lo, int64_t hi) const {
        std::vector<uint32_t> survivors;
        for (const auto& zm : zone_maps_) {
            if (zm.max_key >= lo && zm.min_key <= hi) {
                survivors.push_back(zm.partition_id);
            }
        }
        return survivors;
    }

private:
    uint32_t num_partitions_;
    uint32_t rows_per_partition_;
    int64_t jitter_;
    std::vector<ZoneMap> zone_maps_;
};

}  // namespace dce
