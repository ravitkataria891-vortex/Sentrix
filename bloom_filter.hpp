#pragma once

#include "hash_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace sentrix {

class BloomFilter {
public:
    explicit BloomFilter(std::size_t bit_count = 1'000'003,
                         std::size_t hash_count = 7,
                         std::size_t reset_after_inserts = 500'000)
        : bits_(bit_count, false),
          hash_count_(hash_count),
          reset_after_inserts_(reset_after_inserts) {
        if (bit_count == 0) {
            throw std::invalid_argument("BloomFilter bit_count must be greater than zero");
        }
        if (hash_count == 0) {
            throw std::invalid_argument("BloomFilter hash_count must be greater than zero");
        }
        if (reset_after_inserts_ == 0) {
            throw std::invalid_argument("BloomFilter reset threshold must be greater than zero");
        }
    }

    bool possibly_contains(std::string_view value) const noexcept {
        const auto [h1, h2] = hashes(value);
        for (std::size_t i = 0; i < hash_count_; ++i) {
            const std::size_t index = index_for(h1, h2, i);
            if (!bits_[index]) {
                return false;
            }
        }
        return true;
    }

    void add(std::string_view value) {
        if (insert_count_ >= reset_after_inserts_) {
            reset();
        }
        const auto [h1, h2] = hashes(value);
        for (std::size_t i = 0; i < hash_count_; ++i) {
            bits_[index_for(h1, h2, i)] = true;
        }
        ++insert_count_;
    }

    void reset() noexcept {
        std::fill(bits_.begin(), bits_.end(), false);
        insert_count_ = 0;
        ++reset_count_;
    }

    std::size_t bit_count() const noexcept { return bits_.size(); }
    std::size_t hash_count() const noexcept { return hash_count_; }
    std::size_t insert_count() const noexcept { return insert_count_; }
    std::size_t reset_count() const noexcept { return reset_count_; }

    double estimated_false_positive_rate() const noexcept {
        const double m = static_cast<double>(bits_.size());
        const double k = static_cast<double>(hash_count_);
        const double n = static_cast<double>(insert_count_);
        return std::pow(1.0 - std::exp(-k * n / m), k);
    }

    std::size_t memory_usage_bytes() const noexcept {
        return sizeof(*this) + (bits_.capacity() / 8U);
    }

private:
    std::pair<std::uint32_t, std::uint32_t> hashes(std::string_view value) const noexcept {
        const std::uint32_t h1 = fnv1a(value);
        std::uint32_t h2 = hash_with_seed(value, 0x9e3779b9U);
        if (h2 == 0U) {
            h2 = 0x27d4eb2dU;
        }
        return {h1, h2};
    }

    std::size_t index_for(std::uint32_t h1, std::uint32_t h2, std::size_t i) const noexcept {
        const std::uint64_t combined = static_cast<std::uint64_t>(h1) +
            static_cast<std::uint64_t>(i) * static_cast<std::uint64_t>(h2);
        return static_cast<std::size_t>(combined % bits_.size());
    }

    std::vector<bool> bits_;
    std::size_t hash_count_;
    std::size_t reset_after_inserts_;
    std::size_t insert_count_ = 0;
    std::size_t reset_count_ = 0;
};

} // namespace sentrix