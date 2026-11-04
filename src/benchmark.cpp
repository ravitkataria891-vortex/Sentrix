#include "bloom_filter.hpp"
#include "lru_cache.hpp"
#include "memory_usage.hpp"
#include "pipeline.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kSeed = 0x5E4721A5ULL;
constexpr double kTargetFpr = 0.01;
constexpr std::size_t kCacheCap = 50'000;
constexpr std::size_t kRecentWindow = 32'000;
constexpr std::size_t kProbes = 100'000;

struct Workload {
    std::vector<std::string> payloads;
    std::vector<std::uint8_t> is_dup;
    std::size_t unique_count = 0;
    std::size_t duplicate_count = 0;
};

Workload make_workload(std::size_t count) {
    Workload wl;
    wl.unique_count = std::max<std::size_t>((count * 80U) / 100U, 1);
    wl.duplicate_count = count - wl.unique_count;
    wl.is_dup.assign(count, 0);
    for (std::size_t i = 1; i <= wl.duplicate_count && i < count; ++i) wl.is_dup[i] = 1;

    std::mt19937_64 rng(kSeed);
    if (count > 2) std::shuffle(wl.is_dup.begin() + 1, wl.is_dup.end(), rng);

    wl.payloads.reserve(count);
    std::size_t u = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!wl.is_dup[i]) {
            wl.payloads.push_back("payload-" + std::to_string(u++));
        } else {
            const std::size_t low = ((rng() % 5ULL) != 0ULL && u > kRecentWindow) ? (u - kRecentWindow) : 0;
            std::uniform_int_distribution<std::size_t> dist(low, u - 1);
            wl.payloads.push_back("payload-" + std::to_string(dist(rng)));
        }
    }
    return wl;
}

template <typename Fn>
std::pair<double, decltype(std::declval<Fn>()())> median_timed(int repeats, Fn&& fn) {
    std::vector<double> times;
    decltype(fn()) out{};
    for (int r = 0; r < repeats; ++r) {
        const auto t0 = Clock::now();
        out = fn();
        const auto t1 = Clock::now();
        times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(times.begin(), times.end());
    return {times[times.size() / 2], out};
}

struct Metrics {
    std::size_t hits_or_dups = 0;
    std::size_t false_positives = 0;
    std::size_t evictions = 0;
    std::size_t skipped = 0;
    std::size_t flushes = 0;
    std::size_t bits = 0;
    std::size_t hashes = 0;
    std::size_t threshold = 0;
    double theo_fpr = 0.0;
    double emp_fpr = 0.0;
    std::size_t struct_bytes = 0;
    std::size_t live_rss = 0;
    std::size_t peak_rss = 0;
};

Metrics run_bloom(const Workload& wl, bool extras) {
    const std::size_t m = sentrix::BloomFilter::optimal_bit_count(wl.unique_count, kTargetFpr);
    const std::size_t k = sentrix::BloomFilter::optimal_hash_count(m, wl.unique_count);
    sentrix::BloomFilter bf(m, k, kTargetFpr);

    Metrics out;
    out.bits = bf.bit_count();
    out.hashes = bf.hash_count();
    out.threshold = bf.flush_threshold();
    out.struct_bytes = bf.memory_bytes();

    for (std::size_t i = 0; i < wl.payloads.size(); ++i) {
        if (bf.check_and_add(wl.payloads[i])) {
            ++out.hits_or_dups;
            if (!wl.is_dup[i]) ++out.false_positives;
        }
    }
    out.flushes = bf.flush_count();
    out.theo_fpr = bf.estimated_false_positive_rate();

    if (extras) {
        out.live_rss = sentrix::current_rss_bytes();
        out.peak_rss = sentrix::peak_rss_bytes();
        std::size_t fp = 0;
        for (std::size_t i = 0; i < kProbes; ++i) {
            if (bf.possibly_contains("absent-" + std::to_string(i))) ++fp;
        }
        out.emp_fpr = static_cast<double>(fp) / static_cast<double>(kProbes);
    }
    return out;
}

Metrics run_set(const Workload& wl, bool calc_mem) {
    std::unordered_set<std::string> seen;
    seen.reserve(wl.unique_count);
    Metrics out;
    for (const auto& p : wl.payloads) {
        if (!seen.insert(p).second) ++out.hits_or_dups;
    }
    if (calc_mem) {
        std::size_t heap = 0;
        for (const auto& s : seen) if (s.capacity() > 15) heap += s.capacity() + 1;
        out.struct_bytes = seen.bucket_count() * sizeof(void*) +
                           seen.size() * (sizeof(std::string) + 2 * sizeof(void*)) + heap;
        out.live_rss = sentrix::current_rss_bytes();
        out.peak_rss = sentrix::peak_rss_bytes();
    }
    return out;
}

Metrics run_lru(const Workload& wl, bool calc_mem) {
    sentrix::LRUCache cache(std::min(kCacheCap, wl.unique_count));
    Metrics out;
    for (const auto& p : wl.payloads) {
        if (cache.get(p)) ++out.hits_or_dups;
        else cache.put(p, "res:" + p);
    }
    out.evictions = cache.evictions();
    if (calc_mem) {
        out.struct_bytes = cache.approx_memory_bytes();
        out.live_rss = sentrix::current_rss_bytes();
        out.peak_rss = sentrix::peak_rss_bytes();
    }
    return out;
}

Metrics run_pipeline(const Workload& wl, bool calc_mem) {
    sentrix::SentrixConfig cfg{std::min(kCacheCap, wl.unique_count), wl.unique_count, kTargetFpr};
    sentrix::SentrixPipeline pipe{cfg};
    Metrics out;
    for (const auto& p : wl.payloads) {
        if (pipe.process(p).status == sentrix::StageStatus::RESOLVED) ++out.hits_or_dups;
        else pipe.store_response(p, "res:" + p);
    }
    out.evictions = pipe.cache().evictions();
    out.skipped = pipe.cache_stage().skipped_lookups();
    if (calc_mem) {
        out.struct_bytes = pipe.bloom_filter().memory_bytes() + pipe.cache().approx_memory_bytes();
        out.live_rss = sentrix::current_rss_bytes();
        out.peak_rss = sentrix::peak_rss_bytes();
    }
    return out;
}

Metrics run_map(const Workload& wl, bool calc_mem) {
    std::unordered_map<std::string, std::string> map;
    map.reserve(wl.unique_count);
    Metrics out;
    for (const auto& p : wl.payloads) {
        if (map.find(p) != map.end()) ++out.hits_or_dups;
        else map.emplace(p, "res:" + p);
    }
    if (calc_mem) {
        std::size_t heap = 0;
        for (const auto& [k, v] : map) {
            if (k.capacity() > 15) heap += k.capacity() + 1;
            if (v.capacity() > 15) heap += v.capacity() + 1;
        }
        out.struct_bytes = map.bucket_count() * sizeof(void*) +
                           map.size() * (sizeof(std::pair<const std::string, std::string>) + 2 * sizeof(void*)) + heap;
        out.live_rss = sentrix::current_rss_bytes();
        out.peak_rss = sentrix::peak_rss_bytes();
    }
    return out;
}

// Isolated child process so OS PeakWorkingSetSize / ru_maxrss is uncontaminated across baselines.
int run_memory_child(std::string_view variant, std::size_t count) {
    const Workload wl = make_workload(count);
    const std::size_t base_cur = sentrix::current_rss_bytes();
    const std::size_t base_peak = sentrix::peak_rss_bytes();

    Metrics m;
    if (variant == "bloom") {
        const std::size_t bits = sentrix::BloomFilter::optimal_bit_count(wl.unique_count, kTargetFpr);
        const std::size_t k = sentrix::BloomFilter::optimal_hash_count(bits, wl.unique_count);
        sentrix::BloomFilter bf(bits, k, kTargetFpr);
        for (const auto& p : wl.payloads) (void)bf.check_and_add(p);
        m.live_rss = sentrix::current_rss_bytes();
        m.peak_rss = sentrix::peak_rss_bytes();
    } else if (variant == "set") m = run_set(wl, true);
    else if (variant == "lru") m = run_lru(wl, true);
    else if (variant == "pipeline") m = run_pipeline(wl, true);
    else if (variant == "map") m = run_map(wl, true);
    else return 2;

    const std::size_t delta = std::max(m.live_rss > base_cur ? m.live_rss - base_cur : 0,
                                       m.peak_rss > base_peak ? m.peak_rss - base_peak : 0);
    std::cout << "RSS " << delta << " " << m.peak_rss << "\n";
    return 0;
}

std::string fmt_bytes(std::size_t b) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    if (b >= 1024ULL * 1024ULL) oss << (static_cast<double>(b) / (1024.0 * 1024.0)) << " MiB";
    else if (b >= 1024ULL) oss << (static_cast<double>(b) / 1024.0) << " KiB";
    else oss << b << " B";
    return oss.str();
}

std::string query_rss(const std::string& exe, bool enabled, const char* variant, std::size_t count, std::size_t est) {
    std::string out, tag;
    std::size_t delta = 0, peak = 0;
    if (enabled && sentrix::run_subprocess_capture(exe, {"--memory-child", variant, std::to_string(count)}, out)) {
        std::istringstream iss(out);
        if ((iss >> tag >> delta >> peak) && tag == "RSS") {
            return fmt_bytes(peak) + " (+" + fmt_bytes(delta) + ")";
        }
    }
    return "~" + fmt_bytes(est);
}

std::string backend_compute(std::string_view q, int rounds) {
    std::uint32_t h = sentrix::fnv1a(q);
    for (int i = 0; i < rounds; ++i) {
        h = sentrix::murmur3_32(q, h ^ static_cast<std::uint32_t>(i));
    }
    return std::to_string(h);
}

void run_use_case_experiments(int repeats) {
    constexpr std::size_t kTotal = 50'000;
    constexpr double kApiCostPerCall = 0.002;
    static constexpr std::string_view kFaqs[] = {
        "How do I reset my password?", "Where is my refund?", "Track my order status",
        "Update billing email", "Cancel my subscription", "API rate limit error",
        "Download invoice PDF", "Two-factor login issue"
    };

    auto build_stream = [](std::size_t unique_n, std::size_t hot_pool, const char* prefix) {
        std::vector<std::string> stream;
        stream.reserve(kTotal);
        std::vector<std::uint8_t> dup(kTotal, 0);
        const std::size_t dups = kTotal - unique_n;
        for (std::size_t i = 1; i <= dups && i < kTotal; ++i) dup[i] = 1;
        std::mt19937_64 rng(kSeed + unique_n);
        std::shuffle(dup.begin() + 1, dup.end(), rng);

        std::size_t u = 0;
        for (std::size_t i = 0; i < kTotal; ++i) {
            if (!dup[i]) {
                if (u < hot_pool) {
                    stream.push_back(std::string{kFaqs[u % 8]} + " #" + std::to_string(u));
                } else {
                    stream.push_back(std::string{prefix} + std::to_string(u));
                }
                ++u;
            } else {
                const std::size_t cap = std::min(u, hot_pool);
                std::uniform_int_distribution<std::size_t> d(0, cap - 1);
                const std::size_t pick = d(rng);
                if (pick < hot_pool) {
                    stream.push_back(std::string{kFaqs[pick % 8]} + " #" + std::to_string(pick));
                } else {
                    stream.push_back(std::string{prefix} + std::to_string(pick));
                }
            }
        }
        return stream;
    };

    struct ExpResult {
        double raw_ms = 0.0;
        double sentrix_ms = 0.0;
        std::size_t backend_calls = 0;
        std::size_t saved_calls = 0;
        std::size_t mem_bytes = 0;
    };

    auto run_case = [&](const std::vector<std::string>& reqs, std::size_t unique_n,
                        std::size_t notepad_cap, int work_rounds, sentrix::BloomMode mode) {
        volatile std::uint64_t sink = 0;
        const auto [raw_ms, _r] = median_timed(repeats, [&] {
            std::uint64_t acc = 0;
            for (const auto& q : reqs) acc ^= static_cast<std::uint8_t>(backend_compute(q, work_rounds)[0]);
            sink = acc;
            return acc;
        });

        ExpResult out;
        out.raw_ms = raw_ms;
        const auto [sx_ms, last] = median_timed(repeats, [&] {
            sentrix::SentrixPipeline sp{{notepad_cap, unique_n, kTargetFpr, mode}};
            std::size_t calls = 0;
            std::uint64_t acc = 0;
            for (const auto& q : reqs) {
                const auto res = sp.process(q);
                if (res.status == sentrix::StageStatus::RESOLVED && res.response) {
                    acc ^= static_cast<std::uint8_t>((*res.response)[0]);
                } else if (res.status == sentrix::StageStatus::CONTINUE) {
                    ++calls;
                    std::string ans = backend_compute(q, work_rounds);
                    acc ^= static_cast<std::uint8_t>(ans[0]);
                    sp.store_response(q, std::move(ans));
                }
            }
            sink = acc;
            return std::pair<std::size_t, std::size_t>{
                calls, sp.bloom_filter().memory_bytes() + sp.cache().approx_memory_bytes()};
        });
        (void)_r; (void)sink;
        out.sentrix_ms = sx_ms;
        out.backend_calls = last.first;
        out.saved_calls = reqs.size() - last.first;
        out.mem_bytes = last.second;
        return out;
    };

    const auto helpdesk = run_case(build_stream(20'000, 500, "ticket-"), 20'000, 1'000, 500, sentrix::BloomMode::GateCache);
    const auto retries  = run_case(build_stream(30'000, 30'000, "submit-order-"), 30'000, 0, 500, sentrix::BloomMode::DropDuplicates);
    const auto bad_fit  = run_case(build_stream(49'500, 49'500, "unique-query-"), 49'500, 1'000, 0, sentrix::BloomMode::GateCache);

    std::cout << "\n=== Real-World Use-Case & Trade-Off Experiments (" << kTotal << " requests each) ===\n"
              << std::left << std::setw(34) << "Scenario" << std::right
              << std::setw(12) << "NoSentrix" << std::setw(12) << "Sentrix"
              << std::setw(10) << "Speedup" << std::setw(15) << "BackendCalls"
              << std::setw(13) << "SentrixMem\n";

    auto print_exp = [&](const char* title, const ExpResult& e) {
        std::cout << std::left << std::setw(34) << title << std::right << std::fixed << std::setprecision(2)
                  << std::setw(9) << e.raw_ms << " ms"
                  << std::setw(9) << e.sentrix_ms << " ms"
                  << std::setw(9) << (e.raw_ms / e.sentrix_ms) << "x"
                  << std::setw(9) << e.backend_calls << "/" << (kTotal / 1000) << "k"
                  << std::setw(13) << fmt_bytes(e.mem_bytes) << "\n";
    };
    print_exp("1. AI Help Desk (60% repeats)", helpdesk);
    print_exp("2. Double-Click Retries (40% dup)", retries);
    print_exp("3. 99% Unique + Cheap (Not worth)", bad_fit);

    std::cout << std::fixed << std::setprecision(2)
              << "  -> Help-Desk AI API cost (@ $0.002/call): $" << (kTotal * kApiCostPerCall)
              << " without Sentrix vs $" << (helpdesk.backend_calls * kApiCostPerCall)
              << " with Sentrix (saved $" << (helpdesk.saved_calls * kApiCostPerCall)
              << ", " << helpdesk.saved_calls << " lookups avoided in a 1000-slot notepad).\n"
              << "  -> Retry storm prevented " << retries.saved_calls
              << " duplicate order executions using " << fmt_bytes(retries.mem_bytes) << ".\n"
              << "  -> 99% unique + cheap work ran SLOWER (" << (bad_fit.raw_ms / bad_fit.sentrix_ms)
              << "x), confirming Sentrix is unwarranted without repetition or lookup cost.\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string_view{argv[1]} == "--memory-child") {
        return run_memory_child(argv[2], static_cast<std::size_t>(std::stoull(argv[3])));
    }

    std::vector<std::size_t> sizes;
    int repeats = 3;
    bool measure_rss = true;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--no-memory") measure_rss = false;
        else if (arg == "--repeats" && i + 1 < argc) repeats = std::max(1, std::atoi(argv[++i]));
        else {
            const std::size_t n = static_cast<std::size_t>(std::stoull(std::string{arg}));
            if (n < 10) return 1;
            sizes.push_back(n);
        }
    }
    if (sizes.empty()) sizes = {10'000, 100'000, 1'000'000};

    const std::string exe = sentrix::executable_path(argv[0]);
    std::cout << "Sentrix Benchmark (80% unique / 20% duplicate | median of " << repeats
              << " | target FPR 1% | LRU cap " << kCacheCap << ")\n";

    for (std::size_t n : sizes) {
        const Workload wl = make_workload(n);
        const auto [bf_ms, _b] = median_timed(repeats, [&] { return run_bloom(wl, false); });
        const Metrics bf = run_bloom(wl, true);
        const auto [set_ms, _s] = median_timed(repeats, [&] { return run_set(wl, false); });
        const Metrics set_m = run_set(wl, true);
        const auto [lru_ms, _l] = median_timed(repeats, [&] { return run_lru(wl, false); });
        const Metrics lru = run_lru(wl, true);
        const auto [pipe_ms, _p] = median_timed(repeats, [&] { return run_pipeline(wl, false); });
        const Metrics pipe = run_pipeline(wl, true);
        const auto [map_ms, _m] = median_timed(repeats, [&] { return run_map(wl, false); });
        const Metrics map = run_map(wl, true);
        (void)_b; (void)_s; (void)_l; (void)_p; (void)_m;

        std::cout << "\n=== Scale: " << n << " (" << wl.unique_count << " unique, "
                  << wl.duplicate_count << " duplicates) ===\n"
                  << "[Deduplication]\n"
                  << std::left << std::setw(24) << "Structure" << std::right
                  << std::setw(11) << "Time(ms)" << std::setw(13) << "Flagged"
                  << std::setw(14) << "StructMem" << std::setw(26) << "Peak RSS (+Delta)\n"
                  << std::left << std::setw(24) << "Sentrix BloomFilter" << std::right << std::fixed << std::setprecision(2)
                  << std::setw(11) << bf_ms << std::setw(13) << bf.hits_or_dups
                  << std::setw(14) << fmt_bytes(bf.struct_bytes)
                  << std::setw(26) << query_rss(exe, measure_rss, "bloom", n, bf.struct_bytes) << "\n"
                  << std::left << std::setw(24) << "std::unordered_set" << std::right
                  << std::setw(11) << set_ms << std::setw(13) << set_m.hits_or_dups
                  << std::setw(14) << fmt_bytes(set_m.struct_bytes)
                  << std::setw(26) << query_rss(exe, measure_rss, "set", n, set_m.struct_bytes) << "\n"
                  << std::setprecision(3) << "  Bloom m=" << bf.bits << ", k=" << bf.hashes
                  << ", flush_at=" << bf.threshold << ", flushes=" << bf.flushes
                  << " | FPR theo=" << (bf.theo_fpr * 100.0) << "%, empirical=" << (bf.emp_fpr * 100.0) << "%\n\n"
                  << "[Caching & Pipeline]\n"
                  << std::left << std::setw(24) << "Structure" << std::right
                  << std::setw(11) << "Time(ms)" << std::setw(10) << "Hits" << std::setw(10) << "DupHit%"
                  << std::setw(11) << "Evictions" << std::setw(13) << "StructMem" << std::setw(26) << "Peak RSS (+Delta)\n";

        auto row = [&](const char* name, const char* key, double ms, const Metrics& m) {
            const double pct = wl.duplicate_count ? (100.0 * static_cast<double>(m.hits_or_dups) / static_cast<double>(wl.duplicate_count)) : 0.0;
            std::cout << std::left << std::setw(24) << name << std::right << std::fixed << std::setprecision(2)
                      << std::setw(11) << ms << std::setw(10) << m.hits_or_dups
                      << std::setw(9) << pct << "%" << std::setw(11) << m.evictions
                      << std::setw(13) << fmt_bytes(m.struct_bytes)
                      << std::setw(26) << query_rss(exe, measure_rss, key, n, m.struct_bytes) << "\n";
        };
        row("Sentrix LRUCache", "lru", lru_ms, lru);
        row("Sentrix Pipeline", "pipeline", pipe_ms, pipe);
        row("std::unordered_map", "map", map_ms, map);
        std::cout << "  Bloom skipped " << pipe.skipped << " cache lookups.\n";
    }
    run_use_case_experiments(repeats);
    return 0;
}
