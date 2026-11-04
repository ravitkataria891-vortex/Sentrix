#include "bloom_filter.hpp"
#include "hash_utils.hpp"
#include "lru_cache.hpp"
#include "pipeline.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_run = 0;
int g_failed = 0;

void fail(const char* expr, const char* file, int line) {
    ++g_failed;
    std::cerr << "[FAIL] " << file << ":" << line << " (" << expr << ")\n";
}

// Custom macro instead of assert() so checks remain active under Release (-DNDEBUG).
#define CHECK(expr) do { ++g_run; if (!(expr)) fail(#expr, __FILE__, __LINE__); } while (false)

#define CHECK_THROWS(expr, Exc)                                      \
    do {                                                             \
        ++g_run;                                                     \
        bool ok = false;                                             \
        try { (void)(expr); } catch (const Exc&) { ok = true; } catch (...) {} \
        if (!ok) fail(#expr " throws " #Exc, __FILE__, __LINE__);    \
    } while (false)

void test_hashes() {
    CHECK(sentrix::fnv1a("") == 0x811c9dc5U);
    CHECK(sentrix::fnv1a("a") == 0xe40c292cU);
    CHECK(sentrix::fnv1a("foobar") == 0xbf9cf968U);

    CHECK(sentrix::murmur3_32("", 0U) == 0x00000000U);
    CHECK(sentrix::murmur3_32("", 1U) == 0x514e28b7U);
    CHECK(sentrix::murmur3_32("hello", 0U) == 0x248bfa47U);
    CHECK(sentrix::murmur3_32("Hello, world!", 0x9747b28cU) == 0x24884cbaU);
}

void test_bloom() {
    CHECK_THROWS(sentrix::BloomFilter(0, 5, 0.01), std::invalid_argument);
    CHECK_THROWS(sentrix::BloomFilter(1000, 0, 0.01), std::invalid_argument);
    CHECK_THROWS(sentrix::BloomFilter(1000, 5, 0.0), std::invalid_argument);
    CHECK_THROWS(sentrix::BloomFilter(1000, 5, 1.0), std::invalid_argument);

    {
        const std::size_t m = sentrix::BloomFilter::optimal_bit_count(50'000, 0.01);
        const std::size_t k = sentrix::BloomFilter::optimal_hash_count(m, 50'000);
        const std::size_t th = sentrix::BloomFilter::compute_flush_threshold(m, k, 0.01);
        CHECK(k == 7);
        CHECK(th >= 50'000);
        CHECK(sentrix::BloomFilter::theoretical_fpr(m, k, th) <= 0.01 + 1e-12);
        CHECK(sentrix::BloomFilter::theoretical_fpr(m, k, th + 1) > 0.01);
    }

    {
        sentrix::BloomFilter bf(10'007, 5, 0.01);
        CHECK(!bf.possibly_contains("alpha"));
        CHECK(!bf.check_and_add("alpha"));
        CHECK(bf.possibly_contains("alpha"));
        CHECK(bf.insert_count() == 1);
        CHECK(bf.check_and_add("alpha"));
        bf.add("alpha");
        CHECK(bf.insert_count() == 1);
    }

    {
        sentrix::BloomFilter bf(4'096, 4, 0.01);
        const std::size_t th = bf.flush_threshold();
        CHECK(th > 1 && th < 500);
        std::size_t idx = 0;
        while (bf.insert_count() < th) bf.add("k-" + std::to_string(idx++));
        CHECK(bf.flush_count() == 0);
        CHECK(bf.estimated_false_positive_rate() <= 0.01 + 1e-9);

        std::string trigger;
        while (bf.flush_count() == 0) {
            trigger = "post-" + std::to_string(idx++);
            bf.add(trigger);
        }
        CHECK(bf.flush_count() == 1);
        CHECK(bf.insert_count() == 1);
        CHECK(bf.possibly_contains(trigger));
    }

    {
        const std::size_t m = sentrix::BloomFilter::optimal_bit_count(20'000, 0.01);
        const std::size_t k = sentrix::BloomFilter::optimal_hash_count(m, 20'000);
        sentrix::BloomFilter bf(m, k, 0.01);
        for (std::size_t i = 0; i < 20'000; ++i) bf.add("in-" + std::to_string(i));
        CHECK(bf.flush_count() == 0);

        bool fn = false;
        for (std::size_t i = 0; i < 20'000; ++i) {
            if (!bf.possibly_contains("in-" + std::to_string(i))) { fn = true; break; }
        }
        CHECK(!fn);

        std::size_t fp = 0;
        for (std::size_t i = 0; i < 100'000; ++i) {
            if (bf.possibly_contains("out-" + std::to_string(i))) ++fp;
        }
        const double fpr = static_cast<double>(fp) / 100'000.0;
        CHECK(fpr > 0.004 && fpr < 0.018);
    }
}

void test_lru() {
    {
        sentrix::LRUCache c(2);
        c.put("a", "A");
        c.put("b", "B");
        const auto ga = c.get("a");
        CHECK(ga && *ga == "A");

        c.put("c", "C");
        CHECK(c.contains("a"));
        CHECK(!c.contains("b"));
        CHECK(c.contains("c"));
        CHECK(c.evictions() == 1);
        CHECK(c.size() == 2);

        c.put("a", "A2");
        CHECK(c.size() == 2);
        CHECK(c.evictions() == 1);
        const auto* pa = c.peek("a");
        CHECK(pa && *pa == "A2");

        const auto h = c.hits(), m = c.misses();
        (void)c.peek("a");
        (void)c.peek("missing");
        CHECK(c.hits() == h);
        CHECK(c.misses() == m);
    }

    {
        sentrix::LRUCache z(0);
        z.put("x", "X");
        CHECK(z.size() == 0);
        CHECK(!z.get("x"));

        sentrix::LRUCache o(1);
        o.put("x", "1");
        o.put("y", "2");
        CHECK(!o.contains("x"));
        CHECK(o.get("y").value_or("") == "2");
        CHECK(o.evictions() == 1);
    }

    {
        sentrix::LRUCache deep(200'000);
        for (std::size_t i = 0; i < 200'000; ++i) {
            deep.put("ephemeral-long-key-name-" + std::to_string(i), "v" + std::to_string(i));
        }
        CHECK(deep.size() == 200'000);
        CHECK(deep.get("ephemeral-long-key-name-0").value_or("") == "v0");
        CHECK(deep.get("ephemeral-long-key-name-199999").value_or("") == "v199999");
    }
}

class TraceStage final : public sentrix::Stage {
public:
    TraceStage(std::string_view n, sentrix::StageStatus s, std::vector<std::string_view>& log, std::string r = {})
        : name_(n), status_(s), log_(log), resp_(std::move(r)) {}

    std::string_view name() const noexcept override { return name_; }
    sentrix::StageStatus process(sentrix::Request& req) override {
        log_.push_back(name_);
        if (status_ == sentrix::StageStatus::RESOLVED) req.response = resp_;
        return status_;
    }

private:
    std::string_view name_;
    sentrix::StageStatus status_;
    std::vector<std::string_view>& log_;
    std::string resp_;
};

void test_pipeline() {
    {
        std::vector<std::string_view> log;
        sentrix::Pipeline p;
        p.emplace_stage<TraceStage>("s1", sentrix::StageStatus::CONTINUE, log);
        p.emplace_stage<TraceStage>("s2", sentrix::StageStatus::DROP, log);
        p.emplace_stage<TraceStage>("s3", sentrix::StageStatus::RESOLVED, log, "x");

        const auto res = p.run("k");
        CHECK(res.status == sentrix::StageStatus::DROP);
        CHECK(res.decided_by == "s2");
        CHECK(log.size() == 2);
        CHECK(log[0] == "s1" && log[1] == "s2");
    }

    {
        std::vector<std::string_view> log;
        sentrix::Pipeline p;
        p.emplace_stage<TraceStage>("s1", sentrix::StageStatus::CONTINUE, log);
        p.insert_stage(0, std::make_unique<TraceStage>("s0", sentrix::StageStatus::RESOLVED, log, "hit"));

        const auto r1 = p.run("k");
        CHECK(r1.status == sentrix::StageStatus::RESOLVED);
        CHECK(r1.response.value_or("") == "hit");
        CHECK(log.size() == 1 && log[0] == "s0");

        log.clear();
        p.remove_stage(0);
        const auto r2 = p.run("k");
        CHECK(r2.status == sentrix::StageStatus::CONTINUE);
        CHECK(log.size() == 1 && log[0] == "s1");
    }

    {
        sentrix::SentrixPipeline sp{{4, 1'000, 0.01, sentrix::BloomMode::GateCache}};
        const auto r1 = sp.process("q");
        CHECK(r1.status == sentrix::StageStatus::CONTINUE);
        CHECK(!r1.bloom_maybe_seen);
        CHECK(sp.cache_stage().skipped_lookups() == 1);
        CHECK(sp.cache().misses() == 0);

        sp.store_response("q", "ans");
        CHECK(sp.cached_response("q").value_or("") == "ans");
        CHECK(sp.cache().hits() == 0);

        const auto r2 = sp.process("q");
        CHECK(r2.status == sentrix::StageStatus::RESOLVED);
        CHECK(r2.bloom_maybe_seen);
        CHECK(r2.decided_by == "CacheStage");
        CHECK(r2.response.value_or("") == "ans");
        CHECK(sp.cache().hits() == 1);

        sp.store_response("pre", "ok");
        const auto pre = sp.process("pre");
        CHECK(pre.status == sentrix::StageStatus::RESOLVED);
        CHECK(pre.response.value_or("") == "ok");

        sp.bloom_stage().filter().reset();
        const auto after = sp.process("q");
        CHECK(after.status == sentrix::StageStatus::CONTINUE);
        sp.store_response("q", "ans2");
        CHECK(sp.cache().size() == 2);
        const auto r3 = sp.process("q");
        CHECK(r3.status == sentrix::StageStatus::RESOLVED);
        CHECK(r3.response.value_or("") == "ans2");
    }

    {
        sentrix::SentrixPipeline sp{{4, 1'000, 0.01, sentrix::BloomMode::DropDuplicates}};
        CHECK(sp.process("e").status == sentrix::StageStatus::CONTINUE);
        const auto dup = sp.process("e");
        CHECK(dup.status == sentrix::StageStatus::DROP);
        CHECK(dup.decided_by == "BloomStage");
        CHECK(sp.bloom_stage().dropped_duplicates() == 1);
        CHECK(sp.pipeline().dropped_count() == 1);
    }
}

} // namespace

int main() {
    test_hashes();
    test_bloom();
    test_lru();
    test_pipeline();

    if (g_failed > 0) {
        std::cerr << g_failed << "/" << g_run << " checks FAILED.\n";
        return 1;
    }
    std::cout << "All Sentrix tests passed (" << g_run << " checks).\n";
    return 0;
}
