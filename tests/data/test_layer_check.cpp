// Checking a layer before it takes effect.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>

#include "temp_dir.hpp"

#include <string>
#include <vector>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;
using baker::test::TempDir;

namespace {

const char* const kBase =
    "(shop\n"
    "   (sword (price 10) (damage 4))\n"
    "   (bow (price 12)))\n"
    "(debug (god_mode 0))\n";

Ref<Array> tree(const char* text) {
    return ok(read_string(text, "overlay.dta"));
}

// The refusals, as "line: message" lines, for compact checks.
std::string refusals(const LayerReport& r) {
    std::string out;
    for (const Error& e : r.refusals)
        out += std::to_string(e.line()) + ": " + std::string(e.message()) + "\n";
    return out;
}

}  // namespace

TEST_CASE("check_layer reports the effect and changes nothing") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    const uint64_t epoch = cfg.epoch();
    const LayerReport r = ok(cfg.check_layer("balance", tree("(shop (sword (price 8)))\n")));
    CHECK(r.ok());
    REQUIRE(r.effect.size() == 1);
    CHECK(r.effect[0].path->view() == "shop.sword.price");
    CHECK(to_string(r.effect[0].values) == "8");
    CHECK(to_string(*r.effect[0].below) == "10");
    CHECK(r.effect[0].source.layer == Symbol("balance"));
    CHECK(r.effect[0].below_source->layer == Symbol("default"));
    CHECK(cfg.epoch() == epoch);
    CHECK(cfg.layers().size() == 2);  // default and scratch
    CHECK(cfg.root()->get<int>("shop.sword.price") == 10);
}

TEST_CASE("each rule refuses what it names, located") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));

    const LayerPolicy data{.data_only = true};
    CHECK(refusals(ok(cfg.check_layer(
              "x", tree("(shop\n (sword (price {* 2 5})))\n(debug (god_mode $cheat))\n"), data))) ==
          "2: 'shop.sword.price' holds {* 2 5}; the layer may hold data only\n"
          "3: 'debug.god_mode' holds $cheat; the layer may hold data only\n");

    const LayerPolicy scalars{.scalars_only = true};
    CHECK(refusals(ok(cfg.check_layer(
              "x", tree("(shop (sword (price (1 2))))\n(debug (god_mode #remove))\n"), scalars))) ==
          "1: 'shop.sword.price' is (1 2), not a number, symbol, or string; the layer may only "
          "change values to those\n"
          "2: 'debug.god_mode' is removed; the layer may only change values\n");

    const LayerPolicy existing{.existing_only = true};
    CHECK(
        refusals(ok(cfg.check_layer(
            "x", tree("(shop (axe (price 30)) (bow (price 11)))\n(debug #remove)\n"), existing))) ==
        "1: 'shop.axe.price' is new; the layer may only change values the stack has\n"
        "2: 'debug' is removed; the layer may only change values the stack has\n");

    const Path allowed[] = {Path("shop.sword")};
    const LayerPolicy allow{.allow = allowed};
    CHECK(refusals(ok(
              cfg.check_layer("x", tree("(shop (sword (price 9)) (bow (price 11)))\n"), allow))) ==
          "1: 'shop.bow.price' is outside the paths the layer may change\n");

    // Within every rule at once, a balance change passes.
    const LayerPolicy all{
        .data_only = true, .scalars_only = true, .existing_only = true, .allow = allowed};
    CHECK(ok(cfg.check_layer("x", tree("(shop (sword (price 9) (damage 5)))\n"), all)).ok());
}

TEST_CASE("validators warn over the stack with the layer; warnings_refuse refuses") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    cfg.add_validator([](const ValidatorStack& stack, const Callback<void(const Error&)>& warn) {
        if (stack.root.get<int>("shop.sword.price").value_or(1) <= 0)
            warn(make_error(ErrorCode::Range, "a sword must cost something"));
    });
    const Ref<Array> free_sword = tree("(shop (sword (price 0)))\n");
    const LayerReport r = ok(cfg.check_layer("x", free_sword));
    CHECK(r.ok());
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].message() == "a sword must cost something");
    const LayerReport strict = ok(cfg.check_layer("x", free_sword, {.warnings_refuse = true}));
    CHECK_FALSE(strict.ok());
    CHECK(refusals(strict) == "0: a warning refuses the layer: a sword must cost something\n");
}

TEST_CASE("text is read against the stack's packs, as text only") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    cfg.session().set_pack_levels({"game", "task"});
    REQUIRE(cfg.push_layer(
        "default", dir.write("default.dta", "#pack (shop_data (level game) (version \"1.2\"))\n" +
                                                std::string(kBase))));
    const char* const made_for_newer =
        "#pack (balance_7 (level task) (requires (shop_data \"1.10\")))\n(shop (sword (price "
        "8)))\n";
    const LayerReport r = ok(cfg.check_layer_text("balance_7", made_for_newer));
    CHECK(r.ok());
    REQUIRE(r.warnings.size() == 1);
    CHECK(std::string(r.warnings[0].message()).find("\"1.10\" or later") != std::string::npos);
    CHECK_FALSE(
        ok(cfg.check_layer_text("balance_7", made_for_newer, {.warnings_refuse = true})).ok());
    const char* const made_for_this =
        "#pack (balance_7 (level task) (requires (shop_data \"1.2\")))\n(shop (sword (price 8)))\n";
    CHECK(ok(cfg.check_layer_text("balance_7", made_for_this, {.warnings_refuse = true})).ok());
    // Read as text: the session's earlier warnings are not the candidate's.
    CHECK(ok(cfg.check_layer_text("plain", "(shop (bow (price 9)))\n")).warnings.empty());
    CHECK_FALSE(cfg.check_layer_text("broken", "(shop (bow"));
}

TEST_CASE("push_layer with a policy refuses, changing nothing, or takes the layer") {
    TempDir dir;
    TweakRegistry reg;
    Tweak<int> price{"shop.sword.price", 0, {}, reg};
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    const LayerPolicy policy{.data_only = true, .existing_only = true};
    const uint64_t epoch = cfg.epoch();

    auto refused = cfg.push_layer("bad", tree("(shop (sword (price {cheat})) (axe (price 1)))\n"),
                                  {.policy = &policy});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code() == ErrorCode::Refused);
    CHECK(std::string(refused.error().message()).find("(and 1 more)") != std::string::npos);
    CHECK(cfg.epoch() == epoch);
    CHECK_FALSE(cfg.layer_enabled("bad"));
    CHECK(int(price) == 10);

    // A file layer's #autorun counts as code.
    auto autorun = cfg.push_layer(
        "bad_file", dir.write("bad.dta", "#autorun {give_gold 999}\n(shop (sword (price 9)))\n"),
        {.policy = &policy});
    REQUIRE_FALSE(autorun);
    CHECK(std::string(autorun.error().message()).find("#autorun") != std::string::npos);
    CHECK(cfg.epoch() == epoch);

    REQUIRE(cfg.push_layer("good", tree("(shop (sword (price 8)))\n"), {.policy = &policy}));
    CHECK(int(price) == 8);
    REQUIRE(cfg.push_layer("good_file", dir.write("good.dta", "(shop (sword (price 7)))\n"),
                           {.policy = &policy}));
    CHECK(int(price) == 7);
}

TEST_CASE("a frozen game with live layers takes a checked overlay and refuses a bad one") {
    TempDir dir;
    Config cfg({.live_layers = true, .tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    cfg.freeze();
    const Path shop[] = {Path("shop")};
    const LayerPolicy live{.data_only = true,
                           .scalars_only = true,
                           .existing_only = true,
                           .warnings_refuse = true,
                           .allow = shop};
    CHECK_FALSE(cfg.push_layer("hotfix_bad", tree("(debug (god_mode 1))\n"), {.policy = &live}));
    CHECK(cfg.root()->get<int>("debug.god_mode") == 0);
    REQUIRE(cfg.push_layer("hotfix", tree("(shop (bow (price 14)))\n"), {.policy = &live}));
    CHECK(cfg.root()->get<int>("shop.bow.price") == 14);
    REQUIRE(cfg.remove_layer("hotfix"));
    CHECK(cfg.root()->get<int>("shop.bow.price") == 12);
}

TEST_CASE("on a wide table, the check finds what lookups find") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    std::string base = "(shop\n";
    for (int i = 0; i < 40; ++i)
        base += " (item" + std::to_string(i) + " (price " + std::to_string(i + 1) + "))\n";
    base += " (item5 (price 99)))\n";  // a repeated tag: the first is the one found
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", base)));
    std::string sale = "(shop\n";
    for (int i = 0; i < 40; i += 2)
        sale += " (item" + std::to_string(i) + " (price " + std::to_string(i + 100) + "))\n";
    REQUIRE(cfg.push_layer("sale", dir.write("sale.dta", sale + ")\n")));

    const LayerReport r = ok(cfg.check_layer(
        "x", tree("(shop (item5 (price 7)) (item38 (price 8)) (item41 (price 9)))\n")));
    REQUIRE(r.effect.size() == 3);
    for (const LayerEffectEntry& e : r.effect) {
        CAPTURE(e.path->view());
        if (e.path->view() == "shop.item41.price") {
            CHECK_FALSE(e.below);
            continue;
        }
        const Source s = ok(cfg.source(Path(e.path->view())));
        CHECK(to_string(*e.below) == std::to_string(ok(cfg.get<int>(Path(e.path->view())))));
        CHECK(e.below_source->layer == s.layer);
        CHECK(e.below_source->line == s.line);
    }
    CHECK(to_string(*r.effect[0].below) == "6");
    CHECK(r.effect[1].below_source->layer == Symbol("sale"));
}

TEST_CASE("a validator sees the enabled layers, bottom to top, and a candidate last") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", kBase)));
    REQUIRE(cfg.push_layer("off", tree("(shop (bow (price 1)))\n"), {.enabled = false}));
    REQUIRE(cfg.push_layer("sale", tree("(shop (bow (price 11)))\n")));
    std::vector<const Array*> seen;
    int bow = 0;
    cfg.add_validator([&](const ValidatorStack& stack, const Callback<void(const Error&)>&) {
        seen.clear();
        for (const Ref<Array>& l : stack.layers) seen.push_back(l.get());
        bow = stack.root.get<int>("shop.bow.price").value_or(0);
    });
    REQUIRE(cfg.push_layer("top", tree("(shop (bow (price 13)))\n")));
    REQUIRE(seen.size() == 3);  // "off" is disabled
    CHECK(seen[0] == cfg.layer_root("default").get());
    CHECK(seen[1] == cfg.layer_root("sale").get());
    CHECK(seen[2] == cfg.layer_root("top").get());
    CHECK(bow == 13);
    const Ref<Array> candidate = tree("(shop (bow (price 15)))\n");
    REQUIRE(cfg.check_layer("x", candidate));
    REQUIRE(seen.size() == 4);
    CHECK(seen[3] == candidate.get());
    CHECK(bow == 15);
}

TEST_CASE("a checks_only validator runs in a layer check, never after a merge") {
    Config cfg({.tweaks = nullptr});
    int runs = 0;
    cfg.add_validator([&](const ValidatorStack&, const Callback<void(const Error&)>&) { ++runs; },
                      {.checks_only = true});
    REQUIRE(cfg.push_layer_text("base", "(speed 3)"));
    CHECK(runs == 0);
    (void)ok(cfg.check_layer_text("overlay", "(speed 4)"));
    CHECK(runs == 1);
}

TEST_CASE("a check interns no path: a refused candidate checked 1,000 times adds no names") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta",
                                                "(nopath_shop (nopath_sword (nopath_price 10)))\n"
                                                "(nopath_debug (nopath_god 0))\n")));
    // Names the stack has, in paths it has not: each new, so existing_only refuses them.
    const std::string text =
        "(nopath_debug (nopath_sword (nopath_price 3)))\n"
        "(nopath_shop (nopath_god 1))\n";
    const LayerPolicy policy{.existing_only = true};
    const Ref<Array> candidate = ok(read_string(text, "remote.dta"));

    // A first check interns the names a check reads by (metadata words, the old forms') once.
    const Ref<Array> warm =
        ok(read_string("(nopath_shop (nopath_sword (nopath_price 9)))\n", "remote.dta"));
    REQUIRE(cfg.check_layer("remote", warm, policy)->ok());
    size_t before = Symbol::table_size();
    const LayerReport r = ok(cfg.check_layer("remote", candidate, policy));
    CHECK(Symbol::table_size() == before);  // the first check of it too: its paths are text
    CHECK_FALSE(r.ok());
    REQUIRE(r.effect.size() == 2);
    CHECK(r.effect[0].path->view() == "nopath_debug.nopath_sword.nopath_price");
    CHECK(r.effect[1].path->view() == "nopath_shop.nopath_god");
    CHECK(refusals(r) ==
          "1: 'nopath_debug.nopath_sword.nopath_price' is new; the layer may only change values "
          "the stack has\n"
          "2: 'nopath_shop.nopath_god' is new; the layer may only change values the stack has\n");

    // As text, read each time: the first read interns the layer's name, and then nothing.
    REQUIRE(cfg.check_layer_text("remote", text, policy));
    const LayerChecker checker = cfg.checker();
    REQUIRE(checker.check_text("remote", text, policy));
    before = Symbol::table_size();
    bool refused_each = true;
    for (int i = 0; i < 1000; ++i) {
        refused_each = refused_each && !cfg.check_layer_text("remote", text, policy)->ok();
        refused_each = refused_each && !checker.check_text("remote", text, policy)->ok();
        refused_each = refused_each && !cfg.check_layer("remote", candidate, policy)->ok();
    }
    CHECK(refused_each);
    CHECK(Symbol::table_size() == before);

    // 1,000 different candidates, each refused, each a path of two names the stack has.
    std::string keys;
    for (int i = 0; i < 32; ++i) keys += "(nopath_k" + std::to_string(i) + " 0)\n";
    REQUIRE(cfg.push_layer("keys", dir.write("keys.dta", keys)));
    std::vector<std::string> texts;
    for (int i = 0; i < 1000; ++i)
        texts.push_back("(nopath_k" + std::to_string(i / 32) + " (nopath_k" +
                        std::to_string(i % 32) + " 1))\n");
    REQUIRE(cfg.check_layer_text("remote", texts[0], policy));
    before = Symbol::table_size();
    for (const std::string& t : texts)
        refused_each = refused_each && !cfg.check_layer_text("remote", t, policy)->ok();
    CHECK(refused_each);
    CHECK(Symbol::table_size() == before);
}
