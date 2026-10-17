// The binary cache. Target: a 5 MB tree loads in <= 10 ms with <= 2
// allocations. A cache is the tree as it lies in memory, about 4.3 bytes per byte of
// bench_parse's text, so 1.2 MB of text is the 5 MB tree; 4 MB is bench_parse's own size.
// Rows: the text parse beside it, writing the cache, and the load from a file and from
// memory, with the symbols already interned (warm) and in a fresh process (cold); and
// development's read_cached, which re-hashes the text to know the cache is fresh.
#include <baker/cache.hpp>
#include <baker/config.hpp>
#include <baker/parse.hpp>

#include "alloc_counter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// bench_parse's text, to `bytes`.
std::string make_text(size_t bytes) {
    std::string text;
    int id = 0;
    while (text.size() < bytes) {
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
    return text;
}

// One load in this process, before anything else interns a symbol.
int cold(const char* path) {
    baker::LoadSession session;
    const auto t0 = Clock::now();
    auto r = baker::read_cache(path, session);
    const double ms = ms_since(t0);
    if (!r) return 1;
    std::printf("  cold symbols (fresh process): %.2f ms\n", ms);
    return 0;
}

int run(const char* self, size_t text_bytes) {
    const std::string text = make_text(text_bytes);
    double parse = 1e9;
    baker::Ref<baker::Array> tree;
    for (int rep = 0; rep < 5; ++rep) {
        const auto t0 = Clock::now();
        auto r = baker::read_string(text, "bench.dta");
        parse = std::min(parse, ms_since(t0));
        if (!r) return 1;
        tree = std::move(*r);
    }

    std::string bytes;
    double write = 1e9;
    for (int rep = 0; rep < 5; ++rep) {
        bytes.clear();
        const auto t0 = Clock::now();
        if (!baker::write_cache(*tree, baker::TextSink{[](void* u, std::string_view t) {
                                                           static_cast<std::string*>(u)->append(t);
                                                       },
                                                       &bytes}))
            return 1;
        write = std::min(write, ms_since(t0));
    }
    const std::string path =
        (std::filesystem::temp_directory_path() / "baker_bench_cache.datb").string();
    if (!baker::write_cache(*tree, path)) return 1;

    baker::LoadSession session;
    double from_file = 1e9, from_memory = 1e9, free_ms = 1e9;
    size_t file_allocations = 0, memory_allocations = 0;
    for (int rep = 0; rep < 10; ++rep) {
        baker::test::AllocationScope scope;
        auto t0 = Clock::now();
        auto r = baker::read_cache(path, session);
        from_file = std::min(from_file, ms_since(t0));
        file_allocations = scope.count();
        if (!r || !baker::structurally_equal(*tree, **r)) return 1;
        t0 = Clock::now();
        r = baker::Expected<baker::Ref<baker::Array>>();
        free_ms = std::min(free_ms, ms_since(t0));

        baker::test::AllocationScope memory;
        t0 = Clock::now();
        auto m = baker::read_cache_bytes(bytes, "bench.datb", session);
        from_memory = std::min(from_memory, ms_since(t0));
        memory_allocations = memory.count();
        if (!m) return 1;
    }

    std::printf("%.1f MB of text, a %.1f MB tree\n", static_cast<double>(text.size()) / 1e6,
                static_cast<double>(bytes.size()) / 1e6);
    std::printf("  text parse:      %7.2f ms (%.1f MB/s)\n", parse,
                static_cast<double>(text.size()) / 1e3 / parse);
    std::printf("  write_cache:     %7.2f ms\n", write);
    std::printf("  read_cache file: %7.2f ms, %zu allocations (warm symbols)\n", from_file,
                file_allocations);
    std::printf("  from memory:     %7.2f ms, %zu allocations (warm symbols)\n", from_memory,
                memory_allocations);
    std::printf("  freeing it:      %7.3f ms\n", free_ms);

    // Development's path: the text on disk, read through read_cached.
    const std::string text_path =
        (std::filesystem::temp_directory_path() / "baker_bench_cache.dta").string();
    if (std::FILE* f = std::fopen(text_path.c_str(), "wb")) {
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    std::filesystem::remove(path);
    double miss = 0, hit = 1e9, check = 1e9;
    {
        baker::LoadSession s;
        const auto t0 = Clock::now();
        if (!baker::read_cached(text_path, path, s)) return 1;
        miss = ms_since(t0);
    }
    for (int rep = 0; rep < 5; ++rep) {
        baker::LoadSession s;
        auto t0 = Clock::now();
        if (!baker::cache_fresh(path, s)) return 1;
        check = std::min(check, ms_since(t0));
        t0 = Clock::now();
        if (!baker::read_cached(text_path, path, s)) return 1;
        hit = std::min(hit, ms_since(t0));
    }
    // A Config's file layer, with ConfigOptions::cache_dir.
    const auto cache_dir = std::filesystem::temp_directory_path() / "baker_bench_cache_dir";
    std::filesystem::remove_all(cache_dir);
    std::filesystem::create_directories(cache_dir);
    const std::string dir_name = cache_dir.string();
    double layer_text = 1e9, layer_miss = 0, layer_hit = 1e9;
    for (int rep = 0; rep < 5; ++rep) {
        baker::Config cfg({.edit_mode = false, .tweaks = nullptr});
        const auto t0 = Clock::now();
        if (!cfg.push_layer("bench", text_path)) return 1;
        layer_text = std::min(layer_text, ms_since(t0));
    }
    for (int rep = 0; rep < 6; ++rep) {
        baker::Config cfg({.edit_mode = false, .tweaks = nullptr, .cache_dir = dir_name});
        const auto t0 = Clock::now();
        if (!cfg.push_layer("bench", text_path)) return 1;
        if (rep == 0)
            layer_miss = ms_since(t0);
        else
            layer_hit = std::min(layer_hit, ms_since(t0));
    }
    // A host polling for edits: a reload that finds the large file unchanged reads and hashes
    // all of it.
    double poll_large = 1e9;
    {
        baker::Config cfg({.edit_mode = true, .tweaks = nullptr});
        if (!cfg.push_layer("bench", text_path)) return 1;
        for (int rep = 0; rep < 5; ++rep) {
            const auto t0 = Clock::now();
            if (!cfg.reload()) return 1;
            poll_large = std::min(poll_large, ms_since(t0));
        }
    }
    std::filesystem::remove_all(cache_dir);
    std::filesystem::remove(text_path);
    std::printf("  read_cached, no cache (parse, write): %7.2f ms\n", miss);
    std::printf("  read_cached, fresh (load, re-hash):   %7.2f ms\n", hit);
    std::printf("  cache_fresh:                          %7.2f ms\n", check);
    std::printf("  Config layer, text:                   %7.2f ms\n", layer_text);
    std::printf("  Config layer, cache_dir, no cache:    %7.2f ms\n", layer_miss);
    std::printf("  Config layer, cache_dir, fresh cache: %7.2f ms\n", layer_hit);
    std::printf("  Config reload, unchanged large file:  %7.2f ms\n", poll_large);
    // What a host pays each frame: a reload that finds nothing changed (a small file), and
    // mark_visible with nothing waiting. Best of 5 runs, per call.
    {
        const std::string small =
            (std::filesystem::temp_directory_path() / "baker_bench_small.dta").string();
        if (std::FILE* f = std::fopen(small.c_str(), "wb")) {
            std::fputs("(speed 1)\n", f);
            std::fclose(f);
        }
        baker::Config cfg({.tweaks = nullptr});
        if (!cfg.push_layer("small", small)) return 1;
        double poll = 1e9, visible = 1e9;
        for (int rep = 0; rep < 5; ++rep) {
            auto t0 = Clock::now();
            for (int i = 0; i < 10000; ++i) (void)cfg.reload();
            poll = std::min(poll, ms_since(t0) * 1e6 / 10000);
            t0 = Clock::now();
            for (int i = 0; i < 10000000; ++i) {
                cfg.mark_visible();                                   // BENCH_MARK_VISIBLE
                std::atomic_signal_fence(std::memory_order_seq_cst);  // keep the loop
            }
            visible = std::min(visible, ms_since(t0) * 1e6 / 10000000);
        }
        // A live edit on set's fast path (a scalar already in scratch), as a slider drag
        // makes every frame; the journal records each.
        double edit = 1e9;
        if (!cfg.set("speed", baker::Node(2))) return 1;
        for (int rep = 0; rep < 5; ++rep) {
            const auto t0 = Clock::now();
            for (int i = 0; i < 1000000; ++i)
                if (!cfg.set("speed", baker::Node(i))) return 1;
            edit = std::min(edit, ms_since(t0) * 1e6 / 1000000);
        }
        std::filesystem::remove(small);
        std::printf("  Config set, a scalar in scratch:      %7.1f ns\n", edit);
        std::printf("  Config reload, unchanged small file:  %7.0f ns\n", poll);
        std::printf("  Config mark_visible, nothing waiting: %7.2f ns\n", visible);
    }
    std::fflush(stdout);
    // One sample per process, so three of them: one can land on a busy moment.
    int r = 0;
    for (int rep = 0; rep < 3 && r == 0; ++rep)
        r = std::system(("\"" + std::string(self) + "\" --cold \"" + path + "\"").c_str());
    std::filesystem::remove(path);
    return r == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--cold") return cold(argv[2]);
    for (const size_t kb : {1200, 4096})
        if (run(argv[0], kb * 1024) != 0) return 1;
    return 0;
}
