// Layers a host received as text.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/std_interop.hpp>

#include "temp_dir.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include "dev_only.hpp"
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
    CHECK_FALSE(ok(cfg.reload()));  // no file to change; check_tests: ok
}

BAKER_DEV_TEST_CASE("write-back refuses a value from a text layer") {
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

// --- A layer as text for a peer. ---

TEST_CASE("layer_text writes a layer a peer reads back to the same tree and values") {
    TempDir dir;
    const std::string base = dir.write("base.dta",
                                       "[RATE 0.1]\n"
                                       "(shop (sword (price 12) (rate RATE)) (bow (price 3)))\n"
                                       "(debug (god_mode 1))\n");
    const std::string match =
        dir.write("match.dta",
                  "; a comment the text does not keep\n"
                  "(shop (sword (price 15) (rate RATE) (tiny 1e-300) (exact 0.1)) (bow #remove))\n"
                  "(debug (god_mode #remove))\n");
    Config server({.tweaks = nullptr});
    REQUIRE(server.push_layer("base", base));
    REQUIRE(server.push_layer("match", match));
    const std::string text = ok(layer_text(server, "match"));
    CHECK(text.find("comment") == std::string::npos);
    CHECK(text.find("RATE") == std::string::npos);  // macros expanded, as read
    CHECK(text.find("#remove") != std::string::npos);

    Config client({.tweaks = nullptr});
    REQUIRE(client.push_layer("base", base));
    REQUIRE(client.push_layer_text("received/match", text));
    CHECK(to_dta(*client.layer_root("received/match")) == to_dta(*server.layer_root("match")));
    CHECK(client.digest() == server.digest());
    CHECK(client.get<int>("shop.sword.price") == 15);
    CHECK(client.get<double>("shop.sword.rate") == 0.1);
    CHECK(client.get<double>("shop.sword.tiny") == 1e-300);
    CHECK_FALSE(client.get<int>("shop.bow.price"));
    CHECK_FALSE(client.get<int>("debug.god_mode"));
    // The same tree writes the same bytes.
    CHECK(ok(layer_text(server, "match")) == text);

    std::string none;
    const auto unknown = server.layer_text("nope", string_out(none));
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code() == ErrorCode::NotFound);
}

BAKER_DEV_TEST_CASE("layer_text of scratch carries exactly the live edits") {
    TempDir dir;
    Config server({.tweaks = nullptr});
    REQUIRE(server.push_layer(
        "base", dir.write("base.dta", "(rules (rounds 3) (time 90) (gravity 9.8))\n")));
    CHECK(ok(layer_text(server, "scratch")).empty());
    REQUIRE(server.set(Path("rules.rounds"), 5));
    REQUIRE(server.set(Path("rules.time"), 120));
    REQUIRE(server.set(Path("rules.gravity"), 0.7f));
    REQUIRE(server.clear_scratch("rules.time"));
    const std::string text = ok(layer_text(server, "scratch"));
    CHECK(text.find("rounds 5") != std::string::npos);
    CHECK(text.find("time") == std::string::npos);

    Config client({.tweaks = nullptr});
    REQUIRE(client.push_layer("base", dir.path("base.dta")));
    REQUIRE(client.push_layer_text("received/lobby", text));
    CHECK(client.get<int>("rules.rounds") == 5);
    CHECK(client.get<int>("rules.time") == 90);
    CHECK(client.get<float>("rules.gravity") == 0.7f);  // exactly the tuned float
}

TEST_CASE("a layer with commands is refused by a data-only receiver, located") {
    TempDir dir;
    const std::string base = dir.write("base.dta", "(shop (sword (price 12)))\n");
    Config server({.tweaks = nullptr});
    REQUIRE(server.push_layer("base", base));
    REQUIRE(
        server.push_layer("match", dir.write("match.dta", "(shop\n (sword (price {* 2 5})))\n")));
    const std::string text = ok(layer_text(server, "match"));

    Config client({.tweaks = nullptr});
    REQUIRE(client.push_layer("base", base));
    const LayerPolicy data{.data_only = true};
    const auto r = client.push_layer_text("received/match", text, {.policy = &data});
    REQUIRE_FALSE(r);
    CHECK(to_string(r.error()).find("'shop.sword.price' holds {* 2 5}") != std::string::npos);
    // Located in the text received, which keeps no layout of the server's file.
    CHECK(to_string(r.error()).find("(received/match:1)") != std::string::npos);
}

TEST_CASE("set_layer_text replaces a text layer in place; the same bytes change nothing") {
    TempDir dir;
    Config client({.tweaks = nullptr});
    REQUIRE(client.push_layer("base", dir.write("base.dta", "(rules (rounds 3) (time 90))\n")));
    REQUIRE(client.push_layer_text("received/lobby", "#define LONG\n(rules (rounds 5))\n"));
    REQUIRE(client.push_layer_text("local", "#ifdef LONG\n(rules (time 300))\n#endif\n"));
    CHECK(client.get<int>("rules.time") == 300);

    // Sent again unchanged: the same tree, no reload.
    const Ref<Array> before = client.root();
    const auto record = client.last_reload();  // none in shipping, which keeps no history
    REQUIRE(client.set_layer_text("received/lobby", "#define LONG\n(rules (rounds 5))\n"));
    CHECK(client.root().get() == before.get());
    if (record) CHECK(client.last_reload()->epoch == record->epoch);

    // New text: in place, below "local", which reads again without the macro.
    REQUIRE(client.set_layer_text("received/lobby", "(rules (rounds 7))\n"));
    CHECK(client.get<int>("rules.rounds") == 7);
    CHECK(client.get<int>("rules.time") == 90);
    CHECK(strings(client.layers()) ==
          std::vector<std::string>{"base", "received/lobby", "local", "scratch"});

    // Under a policy, against the layers below it without the old version: a path only the
    // old version had is new, and the refusal changes nothing.
    REQUIRE(client.set_layer_text("received/lobby", "(rules (rounds 8) (bonus 1))\n"));
    const LayerPolicy existing{.existing_only = true};
    const auto refused = client.set_layer_text("received/lobby", "(rules (bonus 2))\n", &existing);
    REQUIRE_FALSE(refused);
    CHECK(to_string(refused.error()).find("'rules.bonus' is new") != std::string::npos);
    CHECK(client.get<int>("rules.rounds") == 8);
    REQUIRE(client.set_layer_text("received/lobby", "(rules (rounds 9))\n", &existing));
    CHECK(client.get<int>("rules.rounds") == 9);

    CHECK(client.set_layer_text("nope", "").error().code() == ErrorCode::NotFound);
    CHECK(client.set_layer_text("base", "").error().code() == ErrorCode::Refused);
    CHECK(client.set_layer_text("scratch", "").error().code() == ErrorCode::Refused);
}
