#pragma once

#include "bloom_filter.hpp"
#include "lru_cache.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sentrix {

enum class StageStatus { DROP, RESOLVED, CONTINUE };

inline const char* to_string(StageStatus status) noexcept {
    switch (status) {
        case StageStatus::DROP: return "DROP";
        case StageStatus::RESOLVED: return "RESOLVED";
        case StageStatus::CONTINUE: return "CONTINUE";
    }
    return "UNKNOWN";
}

struct Request {
    std::string_view key;
    std::string normalized_key;
    std::optional<std::string> response;
    bool bloom_maybe_seen = false;
    bool definitely_new = false;

    explicit Request(std::string_view input_key) noexcept : key(input_key) {}

    void set_normalized_key(std::string rewritten) noexcept {
        normalized_key = std::move(rewritten);
        key = normalized_key;
    }
};

struct PipelineResult {
    StageStatus status = StageStatus::CONTINUE;
    std::optional<std::string> response;
    bool bloom_maybe_seen = false;
    std::string_view decided_by{};
};

class Stage {
public:
    virtual ~Stage() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual StageStatus process(Request& request) = 0;
    virtual void on_response(std::string& /*key*/, std::string& /*response*/) {}
};

// Ordered stage chain; halts immediately at the first non-CONTINUE status.
class Pipeline {
public:
    void add_stage(std::unique_ptr<Stage> stage) {
        if (!stage) throw std::invalid_argument("Null Stage");
        stages_.push_back(std::move(stage));
    }

    void insert_stage(std::size_t index, std::unique_ptr<Stage> stage) {
        if (!stage) throw std::invalid_argument("Null Stage");
        const std::size_t pos = std::min(index, stages_.size());
        stages_.insert(stages_.begin() + static_cast<std::ptrdiff_t>(pos), std::move(stage));
    }

    void remove_stage(std::size_t index) {
        if (index >= stages_.size()) throw std::out_of_range("Stage index out of range");
        stages_.erase(stages_.begin() + static_cast<std::ptrdiff_t>(index));
    }

    template <typename StageT, typename... Args>
    StageT& emplace_stage(Args&&... args) {
        auto stage = std::make_unique<StageT>(std::forward<Args>(args)...);
        StageT& ref = *stage;
        stages_.push_back(std::move(stage));
        return ref;
    }

    PipelineResult run(std::string_view input_key) {
        Request req{input_key};
        for (const auto& stage : stages_) {
            const StageStatus st = stage->process(req);
            if (st != StageStatus::CONTINUE) {
                if (st == StageStatus::DROP) ++dropped_count_;
                else ++resolved_count_;
                return {st, std::move(req.response), req.bloom_maybe_seen, stage->name()};
            }
        }
        ++continued_count_;
        return {StageStatus::CONTINUE, std::nullopt, req.bloom_maybe_seen, {}};
    }

    void record_response(std::string key, std::string response) {
        for (const auto& stage : stages_) {
            stage->on_response(key, response);
        }
    }

    std::size_t stage_count() const noexcept { return stages_.size(); }
    std::size_t dropped_count() const noexcept { return dropped_count_; }
    std::size_t resolved_count() const noexcept { return resolved_count_; }
    std::size_t continued_count() const noexcept { return continued_count_; }

    std::vector<std::string_view> stage_names() const {
        std::vector<std::string_view> names;
        names.reserve(stages_.size());
        for (const auto& s : stages_) names.push_back(s->name());
        return names;
    }

private:
    std::vector<std::unique_ptr<Stage>> stages_;
    std::size_t dropped_count_ = 0;
    std::size_t resolved_count_ = 0;
    std::size_t continued_count_ = 0;
};

enum class BloomMode { GateCache, DropDuplicates };

class BloomStage final : public Stage {
public:
    BloomStage(std::size_t bit_count,
               std::size_t hash_count,
               double target_fpr = 0.01,
               BloomMode mode = BloomMode::GateCache)
        : filter_(bit_count, hash_count, target_fpr), mode_(mode) {}

    std::string_view name() const noexcept override { return "BloomStage"; }

    StageStatus process(Request& request) override {
        const bool seen = filter_.check_and_add(request.key);
        request.bloom_maybe_seen = seen;
        request.definitely_new = !seen;
        if (seen && mode_ == BloomMode::DropDuplicates) {
            ++dropped_duplicates_;
            return StageStatus::DROP;
        }
        return StageStatus::CONTINUE;
    }

    void on_response(std::string& key, std::string& /*response*/) override { filter_.add(key); }

    BloomMode mode() const noexcept { return mode_; }
    void set_mode(BloomMode mode) noexcept { mode_ = mode; }
    std::size_t dropped_duplicates() const noexcept { return dropped_duplicates_; }
    BloomFilter& filter() noexcept { return filter_; }
    const BloomFilter& filter() const noexcept { return filter_; }

private:
    BloomFilter filter_;
    BloomMode mode_;
    std::size_t dropped_duplicates_ = 0;
};

class CacheStage final : public Stage {
public:
    explicit CacheStage(std::size_t capacity) : cache_(capacity) {}

    std::string_view name() const noexcept override { return "CacheStage"; }

    StageStatus process(Request& request) override {
        if (request.definitely_new) {
            ++skipped_lookups_;
            return StageStatus::CONTINUE;
        }
        if (auto hit = cache_.get(request.key)) {
            request.response = std::move(hit);
            return StageStatus::RESOLVED;
        }
        return StageStatus::CONTINUE;
    }

    void on_response(std::string& key, std::string& response) override {
        cache_.put(std::move(key), std::move(response));
    }

    std::size_t skipped_lookups() const noexcept { return skipped_lookups_; }
    LRUCache& cache() noexcept { return cache_; }
    const LRUCache& cache() const noexcept { return cache_; }

private:
    LRUCache cache_;
    std::size_t skipped_lookups_ = 0;
};

struct SentrixConfig {
    std::size_t cache_capacity = 10'000;
    std::size_t expected_elements = 100'000;
    double target_fpr = 0.01;
    BloomMode bloom_mode = BloomMode::GateCache;
    std::size_t bloom_bits = 0;
    std::size_t bloom_hashes = 0;
};

class SentrixPipeline {
public:
    SentrixPipeline() : SentrixPipeline(SentrixConfig{}) {}

    explicit SentrixPipeline(const SentrixConfig& config) {
        const std::size_t expected = std::max<std::size_t>(config.expected_elements, 1);
        const std::size_t m = config.bloom_bits ? config.bloom_bits
                                                : BloomFilter::optimal_bit_count(expected, config.target_fpr);
        const std::size_t k = config.bloom_hashes ? config.bloom_hashes
                                                  : BloomFilter::optimal_hash_count(m, expected);
        bloom_stage_ = &pipeline_.emplace_stage<BloomStage>(m, k, config.target_fpr, config.bloom_mode);
        cache_stage_ = &pipeline_.emplace_stage<CacheStage>(config.cache_capacity);
    }

    SentrixPipeline(std::size_t cache_capacity,
                    std::size_t bloom_bits,
                    std::size_t bloom_hashes,
                    double target_fpr = 0.01,
                    BloomMode mode = BloomMode::GateCache) {
        bloom_stage_ = &pipeline_.emplace_stage<BloomStage>(bloom_bits, bloom_hashes, target_fpr, mode);
        cache_stage_ = &pipeline_.emplace_stage<CacheStage>(cache_capacity);
    }

    SentrixPipeline(const SentrixPipeline&) = delete;
    SentrixPipeline& operator=(const SentrixPipeline&) = delete;
    SentrixPipeline(SentrixPipeline&&) = default;
    SentrixPipeline& operator=(SentrixPipeline&&) = default;

    PipelineResult process(std::string_view key) {
        PipelineResult res = pipeline_.run(key);
        if (pipeline_.stage_count() == 2 && res.status == StageStatus::CONTINUE && !res.bloom_maybe_seen) {
            last_new_miss_key_.assign(key.data(), key.size());
        } else {
            last_new_miss_key_.clear();
        }
        return res;
    }

    void store_response(std::string key, std::string response) {
        if (pipeline_.stage_count() == 2) {
            if (!last_new_miss_key_.empty() && key == last_new_miss_key_) {
                last_new_miss_key_.clear();
                // Unseen key in an un-cleared Bloom filter is guaranteed absent from LRUCache.
                if (!bloom_stage_->filter().has_cleared()) {
                    cache_stage_->cache().put_known_absent(std::move(key), std::move(response));
                } else {
                    cache_stage_->cache().put(std::move(key), std::move(response));
                }
                return;
            }
            last_new_miss_key_.clear();
            bloom_stage_->filter().add(key);
            cache_stage_->cache().put(std::move(key), std::move(response));
            return;
        }
        last_new_miss_key_.clear();
        pipeline_.record_response(std::move(key), std::move(response));
    }

    std::optional<std::string> cached_response(std::string_view key) const {
        const std::string* val = cache_stage_->cache().peek(key);
        return val ? std::optional<std::string>{*val} : std::nullopt;
    }

    Pipeline& pipeline() noexcept { return pipeline_; }
    const Pipeline& pipeline() const noexcept { return pipeline_; }
    BloomStage& bloom_stage() noexcept { return *bloom_stage_; }
    const BloomStage& bloom_stage() const noexcept { return *bloom_stage_; }
    CacheStage& cache_stage() noexcept { return *cache_stage_; }
    const CacheStage& cache_stage() const noexcept { return *cache_stage_; }
    const BloomFilter& bloom_filter() const noexcept { return bloom_stage_->filter(); }
    const LRUCache& cache() const noexcept { return cache_stage_->cache(); }

private:
    Pipeline pipeline_;
    BloomStage* bloom_stage_ = nullptr;
    CacheStage* cache_stage_ = nullptr;
    std::string last_new_miss_key_;
};

} // namespace sentrix
