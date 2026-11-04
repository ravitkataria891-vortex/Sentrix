#include "pipeline.hpp"

#include <cctype>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

class QueryNormalizerStage final : public sentrix::Stage {
public:
    std::string_view name() const noexcept override { return "QueryNormalizerStage"; }

    sentrix::StageStatus process(sentrix::Request& request) override {
        request.set_normalized_key(normalize(request.key));
        return sentrix::StageStatus::CONTINUE;
    }

    void on_response(std::string& key, std::string& /*response*/) override { key = normalize(key); }

private:
    static std::string normalize(std::string_view raw) {
        std::string out;
        out.reserve(raw.size());
        for (unsigned char ch : raw) {
            if (!std::ispunct(ch)) out.push_back(static_cast<char>(std::tolower(ch)));
        }
        static constexpr std::string_view kPrefixes[] = {
            "what is ", "tell me about ", "explain ", "can you explain "
        };
        std::string_view v{out};
        while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
        for (std::string_view p : kPrefixes) {
            if (v.size() > p.size() && v.substr(0, p.size()) == p) {
                v.remove_prefix(p.size());
                break;
            }
        }
        while (!v.empty() && v.back() == ' ') v.remove_suffix(1);
        return std::string{v};
    }
};

void print_summary(const sentrix::SentrixPipeline& sp) {
    std::cout << "  Stages: [";
    const auto names = sp.pipeline().stage_names();
    for (std::size_t i = 0; i < names.size(); ++i) std::cout << (i ? " -> " : "") << names[i];
    std::cout << "]\n  Outcomes: resolved=" << sp.pipeline().resolved_count()
              << ", dropped=" << sp.pipeline().dropped_count()
              << ", continued=" << sp.pipeline().continued_count()
              << "\n  LRU Cache: size=" << sp.cache().size() << "/" << sp.cache().capacity()
              << ", hits=" << sp.cache().hits() << ", misses=" << sp.cache().misses()
              << ", evictions=" << sp.cache().evictions()
              << ", skipped=" << sp.cache_stage().skipped_lookups()
              << "\n  Bloom Filter: bits=" << sp.bloom_filter().bit_count()
              << ", k=" << sp.bloom_filter().hash_count()
              << ", inserts=" << sp.bloom_filter().insert_count()
              << "/" << sp.bloom_filter().flush_threshold()
              << ", flushes=" << sp.bloom_filter().flush_count()
              << ", est. FPR=" << std::scientific << std::setprecision(3)
              << sp.bloom_filter().estimated_false_positive_rate() << std::defaultfloat << "\n";
}

void run_demos() {
    std::cout << "=== 1. AI Prompt Caching (GateCache) ===\n";
    sentrix::SentrixPipeline cache_pipe{{3, 1'000, 0.01, sentrix::BloomMode::GateCache}};
    for (std::string_view p : {"What is Sentrix?", "Explain Bloom Filters", "What is Sentrix?",
                               "Explain LRU cache", "Explain Bloom Filters", "Give me a new example"}) {
        const auto res = cache_pipe.process(p);
        std::cout << "  \"" << p << "\" -> " << sentrix::to_string(res.status);
        if (res.status == sentrix::StageStatus::RESOLVED && res.response) {
            std::cout << " [" << *res.response << "]\n";
        } else {
            std::string ans = "Computed: " + std::string{p};
            std::cout << " -> " << ans << "\n";
            cache_pipe.store_response(std::string{p}, std::move(ans));
        }
    }
    print_summary(cache_pipe);

    std::cout << "\n=== 2. Event Deduplication (DropDuplicates) ===\n";
    sentrix::SentrixPipeline dedup_pipe{{16, 1'000, 0.01, sentrix::BloomMode::DropDuplicates}};
    for (std::string_view e : {"evt-1001", "evt-1002", "evt-1001", "evt-1003", "evt-1002"}) {
        const auto res = dedup_pipe.process(e);
        std::cout << "  " << e << " -> " << sentrix::to_string(res.status) << "\n";
    }
    print_summary(dedup_pipe);

    std::cout << "\n=== 3. Stage 0 Query Normalization Extensibility ===\n";
    sentrix::SentrixPipeline norm_pipe{};
    norm_pipe.pipeline().insert_stage(0, std::make_unique<QueryNormalizerStage>());
    for (std::string_view q : {"What is Bloom Filter?", "Tell me about bloom filter!", "Explain BLOOM FILTER"}) {
        const auto res = norm_pipe.process(q);
        std::cout << "  \"" << q << "\" -> " << sentrix::to_string(res.status) << "\n";
        if (res.status == sentrix::StageStatus::CONTINUE) {
            norm_pipe.store_response(std::string{q}, "O(k) probabilistic set");
        }
    }
    print_summary(norm_pipe);
}

void run_repl() {
    sentrix::SentrixPipeline sp{};
    std::cout << "Sentrix REPL (:stats, :dedup, :cache, :quit)\n";
    for (std::string line; std::cout << "\nsentrix> " && std::getline(std::cin, line);) {
        if (line == ":quit" || line == ":q") break;
        if (line == ":stats") { print_summary(sp); continue; }
        if (line == ":dedup") { sp.bloom_stage().set_mode(sentrix::BloomMode::DropDuplicates); continue; }
        if (line == ":cache") { sp.bloom_stage().set_mode(sentrix::BloomMode::GateCache); continue; }
        if (line.empty()) continue;

        const auto res = sp.process(line);
        std::cout << "[" << sentrix::to_string(res.status) << "] ";
        if (res.status == sentrix::StageStatus::RESOLVED && res.response) {
            std::cout << *res.response << "\n";
        } else if (res.status == sentrix::StageStatus::DROP) {
            std::cout << "Suppressed by " << res.decided_by << "\n";
        } else {
            std::string ans = "Answer(" + line + ")";
            std::cout << ans << "\n";
            sp.store_response(line, std::move(ans));
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view{argv[1]} == "--interactive") {
        run_repl();
        return 0;
    }
    run_demos();
    return 0;
}
