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
    explicit BloomFilter(std::size_t bit_count = 1'000'000,
                         std::size_t hash_count = 7,
                         double target_fpr = 0.01)
        : bits_( (bit_count == 0) ? throw std::invalid_argument("bit_count must be > 0") : bit_count, false ),
          hash_count_( (hash_count == 0) ? throw std::invalid_argument("hash_count must be > 0") : hash_count ),
          target_fpr_( (!(target_fpr > 0.0 && target_fpr < 1.0))
                       ? throw std::invalid_argument("target_fpr must be in (0, 1)") : target_fpr ),
          flush_threshold_(compute_flush_threshold(bit_count, hash_count, target_fpr)) {}

    // Max distinct inserts n keeping (1 - e^(-k*n/m))^k <= p.
    static std::size_t compute_flush_threshold(std::size_t m, std::size_t k, double p) {
        if (m == 0 || k == 0 || !(p > 0.0 && p < 1.0)) {
            throw std::invalid_argument("Invalid BloomFilter parameters");
        }
        const double kd = static_cast<double>(k);
        const double n = -(static_cast<double>(m) / kd) * std::log(1.0 - std::pow(p, 1.0 / kd));
        return (n >= 1.0) ? static_cast<std::size_t>(std::floor(n)) : 1;
    }

    static std::size_t optimal_bit_count(std::size_t n, double p) {
        if (!(p > 0.0 && p < 1.0)) {
            throw std::invalid_argument("target_fpr must be in (0, 1)");
        }
        const double nd = static_cast<double>(std::max<std::size_t>(n, 1));
        const double kd = std::max(1.0, std::round(-std::log(p) / std::log(2.0)));
        const double m = std::ceil((-kd * nd) / std::log(1.0 - std::pow(p, 1.0 / kd)));
        return std::max<std::size_t>(static_cast<std::size_t>(m), 1);
    }

    static std::size_t optimal_hash_count(std::size_t m, std::size_t n) {
        const double ratio = static_cast<double>(m) / static_cast<double>(std::max<std::size_t>(n, 1));
        return std::max<std::size_t>(static_cast<std::size_t>(std::round(ratio * std::log(2.0))), 1);
    }

    static double theoretical_fpr(std::size_t m, std::size_t k, std::size_t n) noexcept {
        if (m == 0) return 1.0;
        const double kd = static_cast<double>(k);
        return std::pow(1.0 - std::exp(-kd * static_cast<double>(n) / static_cast<double>(m)), kd);
    }

    bool possibly_contains(std::string_view value) const noexcept {
        return all_bits_set(probe(value));
    }

    bool check_and_add(std::string_view value) {
        const Probe p = probe(value);
        if (all_bits_set(p)) {
            return true;
        }
        if (insert_count_ >= flush_threshold_) {
            reset();  // Flush before setting bits so the triggering key is retained.
            ++flush_count_;
        }
        set_bits(p);
        ++insert_count_;
        return false;
    }

    void add(std::string_view value) { static_cast<void>(check_and_add(value)); }

    void reset() noexcept {
        std::fill(bits_.begin(), bits_.end(), false);
        insert_count_ = 0;
        ++reset_epoch_;
    }

    std::size_t bit_count() const noexcept { return bits_.size(); }
    std::size_t hash_count() const noexcept { return hash_count_; }
    double target_fpr() const noexcept { return target_fpr_; }
    std::size_t flush_threshold() const noexcept { return flush_threshold_; }
    std::size_t flush_count() const noexcept { return flush_count_; }
    bool has_cleared() const noexcept { return reset_epoch_ > 0; }
    std::size_t insert_count() const noexcept { return insert_count_; }
    std::size_t memory_bytes() const noexcept { return (bits_.size() + 7) / 8; }

    double estimated_false_positive_rate() const noexcept {
        return theoretical_fpr(bits_.size(), hash_count_, insert_count_);
    }

private:
    struct Probe {
        std::size_t start;
        std::size_t step;
    };

    // Kirsch-Mitzenmacher double hashing: index_i = (h1 + i * h2) mod m.
    Probe probe(std::string_view value) const noexcept {
        const std::size_t m = bits_.size();
        std::size_t step = static_cast<std::size_t>(hash_with_seed(value, 0x9e3779b9U)) % m;
        if (step == 0) step = 1;
        return {static_cast<std::size_t>(fnv1a(value)) % m, step};
    }

    bool all_bits_set(Probe p) const noexcept {
        const std::size_t m = bits_.size();
        std::size_t idx = p.start;
        for (std::size_t i = 0; i < hash_count_; ++i) {
            if (!bits_[idx]) return false;
            idx += p.step;
            if (idx >= m) idx -= m;
        }
        return true;
    }

    void set_bits(Probe p) noexcept {
        const std::size_t m = bits_.size();
        std::size_t idx = p.start;
        for (std::size_t i = 0; i < hash_count_; ++i) {
            bits_[idx] = true;
            idx += p.step;
            if (idx >= m) idx -= m;
        }
    }

    std::vector<bool> bits_;
    std::size_t hash_count_;
    double target_fpr_;
    std::size_t flush_threshold_;
    std::size_t insert_count_ = 0;
    std::size_t flush_count_ = 0;
    std::size_t reset_epoch_ = 0;
};

} // namespace sentrix
