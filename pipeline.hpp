#pragma once

#include "bloom_filter.hpp"
#include "lru_cache.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace sentrix {

enum class StageStatus {
    DROP,
    RESOLVED,
    CONTINUE
};

struct PipelineResult {
    StageStatus status = StageStatus::CONTINUE;
    std::optional<std::string> response;
    bool bloom_maybe_seen = false;
};

class SentrixPipeline {
public:
    SentrixPipeline(std::size_t cache_capacity = 10'000,
                    std::size_t bloom_bits = 1'000'003,
                    std::size_t bloom_hashes = 7,
                    std::size_t bloom_reset_after = 500'000)
        : bloom_(bloom_bits, bloom_hashes, bloom_reset_after),
          cache_(cache_capacity) {}

    PipelineResult process(const std::string& key) {
        const bool maybe_seen = bloom_.possibly_contains(key);

        if (!maybe_seen && bloom_.reset_count() == 0) {
            cache_.record_fast_miss();
            bloom_.add(key);
            return {StageStatus::CONTINUE, std::nullopt, false};
        }

        auto cached = cache_.get(key);
        if (cached.has_value()) {
            return {StageStatus::RESOLVED, std::move(cached), maybe_seen};
        }

        bloom_.add(key);
        return {StageStatus::CONTINUE, std::nullopt, maybe_seen};
    }

    void store_response(const std::string& key, std::string response) {
        cache_.put(key, std::move(response));
    }

    std::optional<std::string> cached_response(const std::string& key) {
        return cache_.get(key);
    }

    std::size_t memory_usage_bytes() const noexcept {
        return bloom_.memory_usage_bytes() + cache_.memory_usage_bytes();
    }

    const BloomFilter& bloom_filter() const noexcept { return bloom_; }
    const LRUCache& cache() const noexcept { return cache_; }

private:
    BloomFilter bloom_;
    LRUCache cache_;
};

inline const char* to_string(StageStatus status) noexcept {
    switch (status) {
        case StageStatus::DROP: return "DROP";
        case StageStatus::RESOLVED: return "RESOLVED";
        case StageStatus::CONTINUE: return "CONTINUE";
    }
    return "UNKNOWN";
}

} // namespace sentrix