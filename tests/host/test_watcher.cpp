// The host kit's watcher: exactly the stack's files, by notification or by stamp.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/host/watcher.hpp>

#include "alloc_counter.hpp"
#include "temp_dir.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace baker;
using baker::host::FileStamp;
using baker::host::Watcher;
using baker::test::TempDir;

namespace {

// The files the watcher reports for the next change: waits up to two seconds for the first
// report (notifications arrive promptly, stamps within their 100 ms interval), then keeps
// polling briefly, since Windows reports one save as several notifications (the truncate,
// the write, the size), which may complete in separate polls. Empty if nothing changed.
struct Seen {
    std::vector<std::string> files;
    uint64_t newest_write_ns = 0;
};
Seen next_change(Watcher& w) {
    Seen out;
    for (int i = 0; i < 200 && out.files.empty(); ++i) {
        if (!w.poll()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        for (int settle = 0; settle < 15; ++settle) {
            for (const Symbol c : w.changed())
                out.files.push_back(
                    std::filesystem::path(std::string(c.str())).lexically_normal().string());
            if (w.newest_write_ns() > out.newest_write_ns)
                out.newest_write_ns = w.newest_write_ns();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            (void)w.poll();
        }
    }
    return out;
}

// Polls until the watcher reports a change, or two seconds pass: for stamps, which report
// each change once.
bool first_change(Watcher& w) {
    for (int i = 0; i < 200; ++i) {
        if (w.poll()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// Polls for a while and reports whether anything changed.
bool stays_quiet(Watcher& w) {
    for (int i = 0; i < 30; ++i) {
        if (w.poll()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

bool has(const Seen& seen, const std::string& path) {
    const std::string want = std::filesystem::path(path).lexically_normal().string();
    for (const std::string& c : seen.files)
        if (c == want) return true;
    return false;
}

}  // namespace

TEST_CASE("the watcher sees the stack's files and nothing else") {
    if constexpr (!kShipping) {
        TempDir dir;
        const std::string base = dir.write("data/default.dta", "#include parts.dta\n(speed 1)\n");
        const std::string parts = dir.write("data/parts.dta", "(jump 2)\n");
        Config cfg({.tweaks = nullptr});
        REQUIRE(cfg.push_layer("default", base));
        REQUIRE(cfg.push_layer("local", dir.path("data/local.dta"), {.optional = true}));

        Watcher w;
        const std::string project = dir.write("baker-project.dta", "(project (layers))\n");
        const std::string_view more[] = {project};
        w.watch(cfg.stack_files(), more);
        CHECK(w.watched() == 4);  // default, parts, the missing local, and the project file
        CHECK(w.by_stamp() == 0);
        CHECK(stays_quiet(w));

        // An included file is saved.
        dir.write("data/parts.dta", "(jump 3)\n");
        const Seen saved = next_change(w);
        CHECK(has(saved, parts));
        CHECK(saved.newest_write_ns > 0);
        CHECK(saved.newest_write_ns <= baker::host::file_clock_now_ns());

        // A swap file, and a data file nothing reads, beside them: nothing.
        dir.write("data/.default.dta.swp", "x");
        dir.write("data/unused.dta", "(x 1)\n");
        CHECK(stays_quiet(w));

        // A missing optional layer's file appears; the project file is saved.
        dir.write("data/local.dta", "(speed 9)\n");
        CHECK(has(next_change(w), dir.path("data/local.dta")));
        dir.write("baker-project.dta", "(project (layers (default \"data/default.dta\")))\n");
        CHECK(has(next_change(w), project));

        // A file is removed.
        std::filesystem::remove(dir.path("data/parts.dta"));
        CHECK(has(next_change(w), parts));
    }
}

TEST_CASE("the watcher follows the stack when its files change") {
    if constexpr (!kShipping) {
        TempDir dir;
        const std::string base = dir.write("data/default.dta", "(speed 1)\n");
        Config cfg({.tweaks = nullptr});
        REQUIRE(cfg.push_layer("default", base));
        Watcher w;
        w.watch(cfg.stack_files());
        uint64_t epoch = cfg.stack_files_epoch();

        // The layer now includes a file in another directory: the host watches again when the
        // epoch moves, and an edit there is seen.
        const std::string shared = dir.write("shared/common.dta", "(lives 3)\n");
        dir.write("data/default.dta", "#include ../shared/common.dta\n(speed 1)\n");
        CHECK(has(next_change(w), base));
        REQUIRE(cfg.reload());
        REQUIRE(cfg.stack_files_epoch() != epoch);
        w.watch(cfg.stack_files());
        CHECK(w.watched() == 2);
        dir.write("shared/common.dta", "(lives 4)\n");
        CHECK(has(next_change(w), shared));
    }
}

TEST_CASE("a file whose directory does not exist yet is checked by its stamp") {
    if constexpr (!kShipping) {
        TempDir dir;
        const std::string base = dir.write("default.dta", "(speed 1)\n");
        Config cfg({.tweaks = nullptr});
        REQUIRE(cfg.push_layer("default", base));
        REQUIRE(cfg.push_layer("mine", dir.path("people/me/local.dta"), {.optional = true}));
        Watcher w;
        w.watch(cfg.stack_files());
        CHECK(w.by_stamp() == 1);
        CHECK(stays_quiet(w));
        dir.write("people/me/local.dta", "(speed 2)\n");  // makes the directories too
        CHECK(has(next_change(w), dir.path("people/me/local.dta")));
    }
}

TEST_CASE("a path with a scheme is checked through the host's stamp hook") {
    if constexpr (!kShipping) {
        std::map<std::string, FileStamp> files = {{"user://baker/local.dta", {100, 10, true}}};
        Watcher w;
        w.set_stamp_hook([&](std::string_view path, FileStamp& out) {
            const auto it = files.find(std::string(path));
            if (it == files.end()) return false;
            out = it->second;
            return true;
        });
        const std::string_view paths[] = {"user://baker/local.dta", "user://baker/gone.dta"};
        w.watch({}, paths);
        CHECK(w.by_stamp() == 2);
        CHECK(stays_quiet(w));
        files["user://baker/local.dta"] = {200, 11, true};
        REQUIRE(first_change(w));
        CHECK(w.changed().size() == 1);
        CHECK(w.changed()[0] == Symbol("user://baker/local.dta"));
        CHECK(w.newest_write_ns() == 200);
        files["user://baker/gone.dta"] = {300, 1, true};  // appears
        REQUIRE(first_change(w));
        CHECK(w.changed()[0] == Symbol("user://baker/gone.dta"));
    }
}

TEST_CASE("a host's virtual path is watched as the file it says it is") {
    if constexpr (!kShipping) {
        TempDir dir;
        dir.write("data/tuning.dta", "(speed 3)\n");
        Watcher w;
        // "res://" is the project's root; "pack://" is in an archive, with no file.
        w.set_disk_path_hook([&](std::string_view path, TextSink out) {
            if (path.substr(0, 6) != "res://") return false;
            out(dir.path(""));
            out(path.substr(6));
            return true;
        });
        w.set_stamp_hook([](std::string_view path, FileStamp& out) {
            if (path.substr(0, 7) != "pack://") return false;
            out = {100, 10, true};
            return true;
        });
        const std::string_view paths[] = {"res://data/tuning.dta", "pack://data/base.dta"};
        w.watch({}, paths);
        CHECK(w.watched() == 2);
        CHECK(w.by_stamp() == 1);  // the archive's file only
        CHECK(stays_quiet(w));
        dir.write("data/tuning.dta", "(speed 4)\n");
        const Seen seen = next_change(w);
        CHECK(has(seen, "res://data/tuning.dta"));  // named as the host gave it
        for (const std::string& f : seen.files)     // Windows may report a save more than once
            CHECK(f == seen.files[0]);
        CHECK(seen.newest_write_ns > 0);
    }
}

TEST_CASE("a quiet frame allocates nothing") {
    if constexpr (!kShipping) {
        TempDir dir;
        const std::string base = dir.write("data/default.dta", "#include parts.dta\n");
        dir.write("data/parts.dta", "(jump 2)\n");
        Config cfg({.tweaks = nullptr});
        REQUIRE(cfg.push_layer("default", base));
        REQUIRE(cfg.push_layer("mine", dir.path("missing/local.dta"), {.optional = true}));
        Watcher w;
        w.watch(cfg.stack_files());
        REQUIRE(w.by_stamp() == 1);  // one by notification's directory, one by stamp
        (void)w.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));  // a stamp check falls due
        const test::AllocationScope scope;
        for (int i = 0; i < 5; ++i) CHECK_FALSE(w.poll());
        CHECK(scope.count() == 0);
    }
}

TEST_CASE("under BAKER_SHIPPING the watcher is inert") {
    if constexpr (kShipping) {
        TempDir dir;
        const std::string base = dir.write("default.dta", "(speed 1)\n");
        Config cfg({.tweaks = nullptr});
        REQUIRE(cfg.push_layer("default", base));
        Watcher w;
        w.watch(cfg.stack_files());
        CHECK(w.watched() == 0);
        dir.write("default.dta", "(speed 2)\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK_FALSE(w.poll());
    }
}
