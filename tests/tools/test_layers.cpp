// The layer tools (<baker/tools/layers.hpp>): what a layer sets, what it removes, where each
// value comes from, and two layers compared.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>
#include <baker/tools/layers.hpp>

#include "dev_only.hpp"
#include "ok.hpp"
#include "temp_dir.hpp"

#include <algorithm>
#include <string>

using namespace baker;
using baker::test::ok;
using baker::test::TempDir;

namespace {
Ref<Array> parse(const char* text) {
    auto r = read_string(text, "overlay.dta");
    REQUIRE(r);
    return *r;
}
}  // namespace

TEST_CASE("layer_effect names each removal, and what it removed") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("base", dir.write("base.dta", "(menu (debug_menu 1) (items play))\n")));
    REQUIRE(cfg.push_layer("tu", dir.write("tu.dta", "(menu (debug_menu #remove))\n")));
    const auto effect = ok(tools::layer_effect(cfg, "tu"));
    bool saw = false;
    for (const LayerEffectEntry& e : effect)
        if (e.path == Symbol("menu.debug_menu")) {
            saw = true;
            CHECK(e.value.is(Symbol("#remove")));
            REQUIRE(e.below);
            CHECK(to_string(*e.below) == "1");
        }
    CHECK(saw);
}

TEST_CASE("layer_effect names a generated file's origin and tool") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    dir.write("econ.dta", "(origin \"economy.xlsx\" (tool sheets-export))\n(econ (price 5))\n");
    REQUIRE(
        cfg.push_layer("default", dir.write("default.dta", "(shop (tax 2))\n#include econ.dta\n")));
    const auto effect = ok(tools::layer_effect(cfg, "default"));
    const auto price = std::find_if(effect.begin(), effect.end(), [](const LayerEffectEntry& e) {
        return e.path == Symbol("econ.price");
    });
    REQUIRE(price != effect.end());
    CHECK(price->source.tool == Symbol("sheets-export"));
}

TEST_CASE("the layer tools work in a shipping build too") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("d", parse("(a 1)")));
    CHECK(tools::layer_effect(cfg, "d")->size() == 1);
}

BAKER_DEV_TEST_CASE("layer_effect reports below and shadowed in a three-layer stack") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("base", parse("(a 1) (b 1) (c 1)")));
    REQUIRE(cfg.push_layer("mid", parse("(a 2) (b 2) (d (e 2))")));
    REQUIRE(cfg.push_layer("top", parse("(b 3)")));
    auto effect = tools::layer_effect(cfg, "mid");
    REQUIRE(effect);
    REQUIRE(effect->size() == 3);
    const LayerEffectEntry& a = (*effect)[0];
    CHECK(a.path == "a");
    CHECK(a.value.try_int() == 2);
    REQUIRE(a.below);
    CHECK(a.below->try_int() == 1);
    CHECK(a.below_source->layer == "base");
    CHECK_FALSE(a.shadowed);
    const LayerEffectEntry& b = (*effect)[1];
    CHECK(b.path == "b");
    CHECK(b.shadowed);  // top overrides it
    CHECK(b.below->try_int() == 3);
    CHECK(b.below_source->layer == "top");
    const LayerEffectEntry& e = (*effect)[2];
    CHECK(e.path == "d.e");
    CHECK_FALSE(e.below);  // nothing else sets it
    CHECK_FALSE(e.below_source);
    CHECK(e.source.layer == "mid");
    CHECK(tools::layer_effect(cfg, "nope").error().code() == ErrorCode::NotFound);

    // A disabled layer shadows nothing; one two layers up does; scratch does.
    REQUIRE(cfg.push_layer("higher", parse("(a 4)")));
    REQUIRE(cfg.set_layer_enabled("top", false));
    REQUIRE(cfg.set("d.e", Node(5)));
    effect = tools::layer_effect(cfg, "mid");
    REQUIRE(effect);
    CHECK((*effect)[0].shadowed);  // a: higher, above the disabled top
    CHECK((*effect)[0].below_source->layer == "higher");
    CHECK_FALSE((*effect)[1].shadowed);  // b: only top had it
    CHECK((*effect)[1].below_source->layer == "base");
    CHECK((*effect)[2].shadowed);  // d.e: scratch
    CHECK((*effect)[2].below_source->layer == "scratch");
}

BAKER_DEV_TEST_CASE("diff compares two layers' own values, for A/B") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(
        cfg.push_layer("a", parse("(boss (cadence 1.2) (hp 100)) (only_a 1)"), {.enabled = false}));
    REQUIRE(cfg.push_layer("b", parse("(boss (cadence 0.9) (hp 100) (size 1 2))")));
    auto d = tools::diff(cfg, "a", "b");
    REQUIRE(d);
    REQUIRE(d->size() == 3);
    CHECK((*d)[0].path == "boss.cadence");
    CHECK((*d)[0].a->try_float() == 1.2);
    CHECK((*d)[0].b->try_float() == 0.9);
    CHECK((*d)[0].source_a->layer == "a");
    CHECK((*d)[1].path == "only_a");
    CHECK_FALSE((*d)[1].b);
    CHECK((*d)[2].path == "boss.size");
    CHECK_FALSE((*d)[2].a);
    CHECK(to_string(*(*d)[2].b) == "(1 2)");
}

// --- Change 3: snapshot and restore ---
