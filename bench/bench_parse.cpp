// Parse throughput on generated config-style text. Target: >= 40 MB/s. Also the
// allocations a parse makes and the time to free its tree.
#include <baker/parse.hpp>

#include "alloc_counter.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

int main() {
    std::string text;
    int id = 0;
    while (text.size() < 4 * 1024 * 1024) {
        const std::string n = std::to_string(id++);
        text += "; entry " + n + "\n(entry_" + n + "\n   (name \"Entry number " + n +
                "\")\n"
                "   (enabled TRUE)\n   (position " +
                n +
                ".5 -12.25 0.0)\n"
                "   (color 0.2 0.4 0.8 1.0)\n   (tags fast loud blue)\n"
                "   (limits (min 0) (max 100) (step 0.5))\n"
                "   (on_hit {play_sfx \"hit.wav\"} {game add_score 10}))\n";
    }
    double best = 1e9, best_free = 1e9;
    size_t allocations = 0;
    for (int rep = 0; rep < 10; ++rep) {
        const baker::test::AllocationScope scope;
        const auto t0 = std::chrono::steady_clock::now();
        auto r = baker::read_string(text, "bench.dta");
        const auto t1 = std::chrono::steady_clock::now();
        allocations = scope.count();
        if (!r) return 1;
        best = std::min(best, std::chrono::duration<double>(t1 - t0).count());
        r = baker::Expected<baker::Ref<baker::Array>>();
        const auto t2 = std::chrono::steady_clock::now();
        best_free = std::min(best_free, std::chrono::duration<double>(t2 - t1).count());
    }
    std::printf("parse: %.1f MB in %.2f ms best of 10 -> %.1f MB/s, %zu allocations\n",
                static_cast<double>(text.size()) / 1e6, best * 1e3,
                static_cast<double>(text.size()) / 1e6 / best, allocations);
    std::printf("freeing the tree: %.3f ms best of 10\n", best_free * 1e3);
    return 0;
}
