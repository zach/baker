// The corpus in tests/corpus: every file parses, round-trips, and survives truncation.
#include <doctest.h>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>
#include <baker/write.hpp>

#include "parse_harness.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace baker;
namespace fs = std::filesystem;

namespace {
std::vector<fs::path> corpus_files() {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(BAKER_CORPUS_DIR))
        if (e.path().extension() == ".dta") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    return files;
}
std::string slurp(const fs::path& p) {
    std::string text;
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    REQUIRE(f);
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    return text;
}
}  // namespace

TEST_CASE("every corpus file parses and round-trips") {
    const auto files = corpus_files();
    REQUIRE(files.size() >= 8);
    for (const fs::path& p : files) {
        CAPTURE(p.string());
        LoadSession session;
        auto root = read_file(p.generic_string(), session);
        REQUIRE_MESSAGE(root.has_value(), (root ? std::string() : to_string(root.error())));
        const std::string printed = to_dta(**root);
        auto again = read_string(printed, "printed.dta");
        REQUIRE_MESSAGE(again.has_value(), printed);
        CHECK(structurally_equal(**root, **again));
    }
}

TEST_CASE("corpus spot checks") {
    const std::string dir = BAKER_CORPUS_DIR;
    LoadSession s;
    auto app = read_file(dir + "/config_app.dta", s);
    REQUIRE(app.has_value());
    // config_app sets screen_size and merges the defaults in afterwards.
    CHECK(to_string((*app)->nodes()[0])
              .starts_with(
                  "(renderer (screen_size 1920 1080) (shadows (resolution 4096) (enabled 1)"));
    CHECK(to_string((*app)->nodes()[1]) ==
          "(audio (channels 32) (master_volume 0.8) (sample_rate 48000))");

    LoadSession ui_session;
    auto ui = read_file(dir + "/ui_menu.dta", ui_session);
    REQUIRE(ui.has_value());
    REQUIRE(ui_session.autorun().size() == 1);
    CHECK(to_string(ui_session.autorun()[0]) == "{ui register_screen main_menu}");

    auto odd = read_file(dir + "/odd_tokens.dta");
    REQUIRE(odd.has_value());
    const Array& crlf = *(*odd)->nodes()[0].array();
    CHECK(crlf.line() == 2);
    CHECK((*crlf[2].array())[1].try_str() == "two\r\nlines");
}

TEST_CASE("fuzzer regressions stay fixed") {
    // Inputs the fuzzer once failed on, kept as files so they are never re-broken.
    size_t count = 0;
    for (const auto& e : fs::directory_iterator(BAKER_FUZZ_REGRESSIONS_DIR)) {
        if (e.path().extension() != ".bin") continue;
        CAPTURE(e.path().string());
        CHECK(fuzz::check_parse(slurp(e.path())));
        ++count;
    }
    CHECK(count > 0);
}

TEST_CASE("every prefix of every corpus file is handled cleanly") {
    // Truncation hits every kind of unterminated construct at end of input.
    for (const fs::path& p : corpus_files()) {
        CAPTURE(p.string());
        const std::string text = slurp(p);
        for (size_t n = 0; n <= text.size(); ++n) {
            const bool ok = fuzz::check_parse(std::string_view(text).substr(0, n));
            if (!ok) FAIL_CHECK("property failed at prefix length " << n);
        }
    }
}

TEST_CASE("the lookup harness holds on the corpus") {
    // The fuzzer's lookup properties, over real-shaped files and a spread of paths.
    const char* const paths[] = {"",
                                 "renderer.clear_color.0",
                                 "renderer.shadows",
                                 "menu.'bacon and eggs'.price",
                                 "app/title",
                                 "0.1.2",
                                 "a..b",
                                 "'unterminated"};
    for (const fs::path& p : corpus_files()) {
        CAPTURE(p.string());
        const std::string text = slurp(p);
        for (const char* path : paths) {
            CAPTURE(path);
            CHECK(fuzz::check_lookup(std::string(path) + "\n" + text));
        }
    }
}
