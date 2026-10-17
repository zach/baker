// Config::explain: a path's whole layer chain.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/std_interop.hpp>

#include "temp_dir.hpp"

#include <initializer_list>
#include <string>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;
using baker::test::TempDir;

namespace {

// One entry, as "layer standing value file:line", for compact comparisons.
std::string show(const ExplainEntry& e) {
    static const char* const kStanding[] = {"wins", "shadowed", "removes", "disabled"};
    std::string file(e.source.file.str());
    if (const auto slash = file.find_last_of('/'); slash != std::string::npos)
        file = file.substr(slash + 1);
    return std::string(e.layer.str()) + " " + kStanding[int(e.standing)] + " " +
           to_string(e.value) +
           (file.empty() ? "" : " " + file + ":" + std::to_string(e.source.line));
}

// The chain as one line, entries separated by " | ".
std::string chain(const Config& cfg, const char* path) {
    auto r = cfg.explain(path);
    REQUIRE(r);
    std::string out;
    for (const ExplainEntry& e : *r) out += (out.empty() ? "" : " | ") + show(e);
    return out;
}

std::string Lines(std::initializer_list<const char*> entries) {
    std::string out;
    for (const char* e : entries) out += (out.empty() ? "" : " | ") + std::string(e);
    return out;
}

const char* const kDefault =
    "(box\n"
    "   (width 2)\n"
    "   (height 1))\n"
    "(cheats (god_mode 1) (noclip 1))\n"
    "(phase 10 20 30)\n";

}  // namespace

TEST_CASE("explain lists every layer that sets a path, bottom to top") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kDefault)));
    REQUIRE(cfg.push_layer("platform", dir.write("platform.dta", "(box (height 5))\n")));
    REQUIRE(cfg.push_layer("local", dir.write("local.dta", "\n(box (width 3))\n")));

    CHECK(chain(cfg, "box.width") ==
          Lines({"default shadowed 2 default.dta:2", "local wins 3 local.dta:2"}));
    CHECK(chain(cfg, "box.height") ==
          Lines({"default shadowed 1 default.dta:3", "platform wins 5 platform.dta:1"}));
    // A value of several nodes is packed, as layer_effect packs it.
    CHECK(chain(cfg, "phase") == Lines({"default wins (10 20 30) default.dta:5"}));
    // An element of it, by position.
    CHECK(chain(cfg, "phase.1") == Lines({"default wins 20 default.dta:5"}));
}

TEST_CASE("the contributor that wins is the one source() names, for every leaf") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kDefault)));
    REQUIRE(cfg.push_layer(
        "dlc", dir.write("dlc.dta", "(box (width 4) (depth 9))\n(cheats (noclip #remove))\n")));
    REQUIRE(cfg.push_layer("patch", dir.write("patch.dta", "(box (depth 7))\n(phase 1 2)\n")));
    // Each leaf, and the value get() returns for it, packed as explain packs it.
    const std::pair<const char*, const char*> leaves[] = {
        {"box.width", "4"}, {"box.height", "1"}, {"box.depth", "7"}, {"cheats.god_mode", "1"},
        {"phase", "(1 2)"}, {"phase.0", "1"},    {"phase.1", "2"}};
    for (const auto& [p, value] : leaves) {
        CAPTURE(p);
        const auto entries = cfg.explain(p);
        REQUIRE(entries);
        const ExplainEntry* winner = nullptr;
        for (const ExplainEntry& e : *entries)
            if (e.standing == Standing::Wins) {
                CHECK(winner == nullptr);
                winner = &e;
            }
        REQUIRE(winner);
        const Source s = ok(cfg.source(p));
        CHECK(winner->layer == s.layer);
        CHECK(winner->source.file == s.file);
        CHECK(winner->source.line == s.line);
        CHECK(to_string(winner->value) == value);
    }
    CHECK(cfg.root()->get<int>("box.width") == 4);
    CHECK(cfg.root()->get<int>("phase.1") == 2);
    CHECK_FALSE(cfg.root()->get<int>("cheats.noclip"));
    const auto noclip = ok(cfg.explain("cheats.noclip"));
    CHECK(noclip[noclip.size() - 1].standing == Standing::Removes);
}

TEST_CASE("a removal is listed with its line, for the key and everything under it") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kDefault)));
    REQUIRE(cfg.push_layer("patch", dir.write("patch.dta", "(cheats\n   (god_mode #remove))\n")));
    CHECK(chain(cfg, "cheats.god_mode") ==
          Lines({"default shadowed 1 default.dta:4", "patch removes '#remove' patch.dta:2"}));
    CHECK_FALSE(cfg.root()->get<int>("cheats.god_mode"));

    // The marker symbol prints quoted when it stands alone. A removed ancestor
    // removes its leaves.
    REQUIRE(cfg.push_layer("cut", dir.write("cut.dta", "(cheats #remove)\n")));
    CHECK(chain(cfg, "cheats.noclip") ==
          Lines({"default shadowed 1 default.dta:4", "cut removes '#remove' cut.dta:1"}));
    CHECK(chain(cfg, "cheats.god_mode") ==
          Lines({"default shadowed 1 default.dta:4", "patch shadowed '#remove' patch.dta:2",
                 "cut removes '#remove' cut.dta:1"}));

    // A layer above sets the key again, and wins.
    REQUIRE(cfg.push_layer("dlc", dir.write("dlc.dta", "(cheats (noclip 0))\n")));
    CHECK(chain(cfg, "cheats.noclip") ==
          Lines({"default shadowed 1 default.dta:4", "cut shadowed '#remove' cut.dta:1",
                 "dlc wins 0 dlc.dta:1"}));
}

TEST_CASE("a tweak's code default is the first entry, and wins only when nothing sets it") {
    TempDir dir;
    TweakRegistry reg;
    Tweak<float> width{"box.width", 0.5f, {}, reg};
    Tweak<int> lives{"player.lives", 3, {}, reg};
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kDefault)));
    CHECK(chain(cfg, "box.width") == Lines({"code shadowed 0.5", "default wins 2 default.dta:2"}));
    CHECK(chain(cfg, "player.lives") == Lines({"code wins 3"}));
}

TEST_CASE("a path nothing sets, or one that holds arrays, is refused") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kDefault)));
    auto missing = cfg.explain("box.depth");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code() == ErrorCode::NotFound);
    auto subtree = cfg.explain("box");
    REQUIRE_FALSE(subtree);
    CHECK(subtree.error().code() == ErrorCode::Refused);
    CHECK(subtree.error().message().find("holds arrays") != std::string_view::npos);
    CHECK_FALSE(cfg.explain(""));
    CHECK_FALSE(cfg.explain("box..width"));
}

#if !BAKER_SHIPPING
TEST_CASE("a disabled layer and scratch are listed, and toggling changes who wins") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kDefault)));
    REQUIRE(cfg.push_layer("experiment", dir.write("exp.dta", "(box (width 3))\n")));
    CHECK(cfg.toggle_layer("experiment") == false);
    CHECK(chain(cfg, "box.width") ==
          Lines({"default wins 2 default.dta:2", "experiment disabled 3 exp.dta:1"}));

    REQUIRE(cfg.set("box.width", 2.5f));
    CHECK(chain(cfg, "box.width") ==
          Lines({"default shadowed 2 default.dta:2", "experiment disabled 3 exp.dta:1",
                 "scratch wins 2.5"}));
    CHECK(cfg.toggle_layer("scratch") == false);
    CHECK(chain(cfg, "box.width") ==
          Lines({"default wins 2 default.dta:2", "experiment disabled 3 exp.dta:1",
                 "scratch disabled 2.5"}));
    CHECK(cfg.toggle_layer("scratch") == true);
    CHECK(cfg.toggle_layer("experiment") == true);
    CHECK(chain(cfg, "box.width") ==
          Lines({"default shadowed 2 default.dta:2", "experiment shadowed 3 exp.dta:1",
                 "scratch wins 2.5"}));
}
#endif
