// Layers a host received as text.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/std_interop.hpp>

#include "temp_dir.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;
using baker::test::TempDir;
namespace fs = std::filesystem;

namespace {

const char* const kBase =
    "(pack game_data (level game) (version \"1.2\"))\n"
    "(event (xp 1) (drops 5))\n";

const char* const kOverlay =
    "(pack double_xp (level live) (requires (game_data \"1.2\")))\n"
    "(origin \"liveops ticket 7\" (tool publisher))\n"
    "(event (xp 2))\n";

bool has_pack(const Config& cfg, const char* name) {
    for (const Pack& p : cfg.read_session().packs())
        if (p.name == Symbol(name)) return true;
    return false;
}

}  // namespace

TEST_CASE("push_layer_text records the pack and origin, and removing it removes them") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    cfg.session().set_pack_levels({"game", "live"});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    REQUIRE(cfg.push_layer_text("received/double_xp@1.dta", kOverlay));
    CHECK(cfg.get<int>("event.xp") == 2);
    CHECK(has_pack(cfg, "double_xp"));
    const Source s = ok(cfg.source("event.xp"));
    CHECK(s.layer == Symbol("received/double_xp@1.dta"));
    CHECK(s.file == Symbol("received/double_xp@1.dta"));
    CHECK(s.line == 3);
    CHECK(s.origin == Symbol("liveops ticket 7"));
    CHECK(s.tool == Symbol("publisher"));
    REQUIRE(cfg.remove_layer("received/double_xp@1.dta"));
    CHECK(cfg.get<int>("event.xp") == 1);
    CHECK_FALSE(has_pack(cfg, "double_xp"));
}

TEST_CASE("push_layer_text never writes or reads a cache") {
    TempDir dir;
    fs::create_directories(dir.path("cache"));
    const std::string cache_dir = dir.path("cache");
    // A file at the path the received layer is named after, so that a cache of that name
    // exists: the text must win over it.
    const std::string named = dir.write("received.dta", "(event (xp 9))\n");
    int writes = 0;
    auto make = [&] {
        auto cfg = std::make_unique<Config>(
            ConfigOptions{.edit_mode = false, .tweaks = nullptr, .cache_dir = cache_dir});
        cfg->set_writer([&](std::string_view path, std::string_view text) {
            ++writes;
            std::ofstream(std::string(path), std::ios::binary) << text;
            return true;
        });
        return cfg;
    };
    if constexpr (!kShipping) {  // shipping writes no caches; plant one here
        auto planter = make();
        REQUIRE(planter->push_layer("x", named));
        REQUIRE(writes == 1);
    }
    const int before = writes;
    auto cfg = make();
    REQUIRE(cfg->push_layer_text(named, "(event (xp 2))\n"));
    CHECK(cfg->get<int>("event.xp") == 2);
    CHECK(writes == before);
}

TEST_CASE("received text may include no files") {
    TempDir dir;
    dir.write("secret.dta", "(event (xp 99))\n");
    Config cfg({.tweaks = nullptr});
    cfg.session().set_pack_levels({"game", "live"});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    const std::string name = dir.path("received.dta");  // its includes would resolve here
    const struct {
        const char* text;
        int line;
    } cases[] = {{"(event (xp 2))\n#include secret.dta\n", 2},
                 {"(event (xp 2))\n#include_opt secret.dta\n", 2},
                 {"(event (xp 2))\n#require secret.dta\n", 2},
                 {"(event\n (xp 2) #merge secret.dta)\n", 2}};
    for (const auto& [text, line] : cases) {
        CAPTURE(text);
        auto pushed = cfg.push_layer_text(name, text);
        REQUIRE_FALSE(pushed);
        CHECK(pushed.error().code() == ErrorCode::Refused);
        CHECK(pushed.error().line() == line);
        CHECK(std::string(pushed.error().message()).find("may include no files") !=
              std::string::npos);
        CHECK_FALSE(cfg.check_layer_text(name, text));
    }
    CHECK(cfg.get<int>("event.xp") == 1);
    CHECK(cfg.layers().size() == 2);
}

TEST_CASE("a text layer is read again when a layer below it changes") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", "#define WIDE\n(w 0)\n")));
    REQUIRE(cfg.push_layer_text("received", "#ifdef WIDE\n(w 2)\n#else\n(w 1)\n#endif\n"));
    CHECK(cfg.get<int>("w") == 2);
    REQUIRE(
        cfg.push_layer("narrow", dir.write("narrow.dta", "#undef WIDE\n"), {.below = "received"}));
    CHECK(cfg.get<int>("w") == 1);
    REQUIRE(cfg.remove_layer("narrow"));
    CHECK(cfg.get<int>("w") == 2);
    CHECK_FALSE(ok(cfg.reload()));  // it has no file to change
}

TEST_CASE("write-back refuses a value from a text layer") {
    if constexpr (kShipping) return;  // write-back is inert in shipping
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    cfg.session().set_pack_levels({"game", "live"});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    REQUIRE(cfg.push_layer_text("received", "(event (xp 2))\n"));
    REQUIRE(cfg.set("event.xp", Node(int64_t{3})));
    auto written = cfg.write_back(Path("event.xp"));
    REQUIRE_FALSE(written);
    CHECK(std::string(written.error().message()).find("pushed as text") != std::string::npos);
}

TEST_CASE("a push with a policy sees the warnings its read adds") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    cfg.session().set_pack_levels({"game", "live"});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    const char* const newer =
        "(pack season_two (level live) (requires (game_data \"1.3\")))\n(event (xp 3))\n";
    const LayerPolicy strict{.data_only = true, .warnings_refuse = true};
    auto pushed = cfg.push_layer_text("season_two", newer, {.policy = &strict});
    REQUIRE_FALSE(pushed);
    CHECK(std::string(pushed.error().message()).find("\"1.3\" or later") != std::string::npos);
    // By path too.
    CHECK_FALSE(
        cfg.push_layer("season_two", dir.write("season_two.dta", newer), {.policy = &strict}));
    CHECK(cfg.get<int>("event.xp") == 1);
    REQUIRE(cfg.push_layer_text("double_xp", kOverlay, {.policy = &strict}));
    CHECK(cfg.get<int>("event.xp") == 2);
}

TEST_CASE("a frozen game with live layers takes received text") {
    TempDir dir;
    Config cfg({.live_layers = true, .tweaks = nullptr});
    cfg.session().set_pack_levels({"game", "live"});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    cfg.freeze();
    const Path event[] = {Path("event")};
    const LayerPolicy live{
        .data_only = true, .scalars_only = true, .warnings_refuse = true, .allow = event};
    REQUIRE(cfg.push_layer_text("double_xp", kOverlay, {.policy = &live}));
    CHECK(cfg.get<int>("event.xp") == 2);
    REQUIRE(cfg.remove_layer("double_xp"));
    CHECK(cfg.get<int>("event.xp") == 1);
}

TEST_CASE("a push takes a report that still holds, and checks again when it does not") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    cfg.session().set_pack_levels({"game", "live"});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    int runs = 0;  // a validator runs once per check, and once after every merge
    cfg.add_validator(
        [&runs](const ValidatorStack&, const Callback<void(const Error&)>&) { ++runs; });
    const LayerPolicy policy{.data_only = true, .warnings_refuse = true};
    const LayerPolicy other{.data_only = true};

    // Each push's cost in validator runs: 1 is the merge alone, 2 a check as well.
    auto cost = [&](auto&& push) {
        const int before = runs;
        REQUIRE(push());
        REQUIRE(cfg.remove_layer("double_xp"));
        return runs - before - 1;  // less the removal's merge
    };
    auto checked = [&] { return ok(cfg.check_layer_text("double_xp", kOverlay, policy)); };
    auto push = [&](const LayerReport& r, const LayerPolicy& p = {}, std::string_view text = {},
                    LayerOptions o = {}) {
        o.policy = p.data_only ? &p : &policy;
        o.checked = &r;
        return cfg.push_layer_text("double_xp", text.empty() ? kOverlay : text, o).has_value();
    };

    LayerReport r = checked();
    CHECK(cost([&] { return push(r); }) == 1);  // held: no second check
    CHECK(cost([&] { return push(r); }) == 2);  // the push and removal changed the stack

    auto stale_after = [&](auto&& change) {
        const LayerReport report = checked();
        change();
        return cost([&] { return push(report); }) == 2;
    };
    CHECK(stale_after(
        [&] { REQUIRE(cfg.push_layer("x", dir.write("x.dta", "(event (drops 6))\n"))); }));
    CHECK(stale_after([&] { REQUIRE(cfg.remove_layer("x")); }));
    CHECK(stale_after([&] { cfg.remove_validator(cfg.add_validator(nullptr)); }));
    CHECK(stale_after([&] { (void)cfg.session(); }));
    if constexpr (!kShipping) {  // toggles and set() are inert in shipping
        CHECK(stale_after([&] { REQUIRE(cfg.toggle_layer("default")); }));
        REQUIRE(cfg.toggle_layer("default"));
        REQUIRE(cfg.set("event.drops", Node(int64_t{6})));  // the next is the fast path
        CHECK(stale_after([&] { REQUIRE(cfg.set("event.drops", Node(int64_t{7}))); }));
        REQUIRE(cfg.clear_scratch());
    }
    // Another text, policy, or place; another Config's report.
    r = checked();
    CHECK(cost([&] { return push(r, {}, "(event (xp 3))\n"); }) == 2);
    r = checked();
    CHECK(cost([&] { return push(r, other); }) == 2);
    // Below the base, its pack's requires is unmet, which `other` lets pass.
    r = ok(cfg.check_layer_text("double_xp", kOverlay, other));
    CHECK(cost([&] { return push(r, other); }) == 1);
    r = ok(cfg.check_layer_text("double_xp", kOverlay, other));
    CHECK(cost([&] { return push(r, other, {}, {.below = "default"}); }) == 2);
    Config twin({.tweaks = nullptr});
    twin.session().set_pack_levels({"game", "live"});
    REQUIRE(twin.push_layer("default", dir.write("default.dta", kBase)));
    r = ok(twin.check_layer_text("double_xp", kOverlay, policy));
    CHECK(cost([&] { return push(r); }) == 2);

    // A tree: the same one, then a different one.
    const Ref<Array> tree = ok(read_string("(event (xp 4))\n", "double_xp"));
    r = ok(cfg.check_layer("double_xp", tree, policy));
    CHECK(cost([&] {
              return cfg.push_layer("double_xp", tree, {.policy = &policy, .checked = &r})
                  .has_value();
          }) == 1);
    r = ok(cfg.check_layer("double_xp", tree, policy));
    const Ref<Array> copy = ok(read_string("(event (xp 4))\n", "double_xp"));
    CHECK(cost([&] {
              return cfg.push_layer("double_xp", copy, {.policy = &policy, .checked = &r})
                  .has_value();
          }) == 2);
}

TEST_CASE("a report that holds and refuses refuses at once, changing nothing") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    cfg.session().set_pack_levels({"game", "live"});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    int runs = 0;
    cfg.add_validator(
        [&runs](const ValidatorStack&, const Callback<void(const Error&)>&) { ++runs; });
    const LayerPolicy policy{.data_only = true};
    const char* const code = "(event (xp {random 1 9}))\n";
    const LayerReport r = ok(cfg.check_layer_text("cheat", code, policy));
    REQUIRE_FALSE(r.ok());
    const uint64_t epoch = cfg.epoch();
    const int before = runs;
    auto pushed = cfg.push_layer_text("cheat", code, {.policy = &policy, .checked = &r});
    REQUIRE_FALSE(pushed);
    CHECK(pushed.error().code() == ErrorCode::Refused);
    CHECK(runs == before);
    CHECK(cfg.epoch() == epoch);
    CHECK(cfg.layers().size() == 2);
}
