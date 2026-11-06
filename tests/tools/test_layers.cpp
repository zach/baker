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
            CHECK(e.values[0].is(Symbol("#remove")));
            REQUIRE(e.below);
            CHECK(to_string(*e.below) == "1");
        }
    CHECK(saw);
}

TEST_CASE("layer_effect names a generated file's origin and tool") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    dir.write("econ.dta", "#origin (\"economy.xlsx\" (tool sheets-export))\n(econ (price 5))\n");
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
    CHECK(a.values[0].try_int() == 2);
    REQUIRE(a.below);
    CHECK((*a.below)[0].try_int() == 1);
    CHECK(a.below_source->layer == "base");
    CHECK_FALSE(a.shadowed);
    const LayerEffectEntry& b = (*effect)[1];
    CHECK(b.path == "b");
    CHECK(b.shadowed);  // top overrides it
    CHECK((*b.below)[0].try_int() == 3);
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
    CHECK((*(*d)[0].a)[0].try_float() == 1.2);
    CHECK((*(*d)[0].b)[0].try_float() == 0.9);
    CHECK((*d)[0].source_a->layer == "a");
    CHECK((*d)[1].path == "only_a");
    CHECK_FALSE((*d)[1].b);
    CHECK((*d)[2].path == "boss.size");
    CHECK_FALSE((*d)[2].a);
    CHECK(to_string(*(*d)[2].b) == "1 2");
}

// --- Change 3: snapshot and restore ---

TEST_CASE("the layer tools name each verb: effect, diff and explain") {
    TempDir dir;
    const char* cancels = "fighters.kestrel.moves.jab.cancels";
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer(
        "default",
        dir.write("default.dta",
                  "(fighters (kestrel (moves (jab (cancels jab low_kick) (damage 30)))))\n")));
    REQUIRE(cfg.push_layer(
        "dlc",
        dir.write("dlc.dta", "(fighters (kestrel (moves (jab (cancels #append kick)))))\n")));
    REQUIRE(cfg.push_layer("agent", dir.write("agent.dta",
                                              "(fighters (kestrel (moves (jab (cancels #append "
                                              "palm) (damage 32 (@was 31))))))\n")));
    CHECK(to_string(*cfg.root()->try_get(cancels)) == "(cancels jab low_kick kick palm)");

    // Each package's effect is its verb and operands over the value below it; a stale pin
    // says so.
    const auto effect = ok(tools::layer_effect(cfg, "agent"));
    REQUIRE(effect.size() == 2);
    CHECK(effect[0].op == Symbol("#append"));
    CHECK(to_string(effect[0].values) == "palm");
    CHECK(to_string(*effect[0].below) == "jab low_kick kick");
    CHECK_FALSE(effect[0].stale);
    CHECK(effect[1].op.empty());
    CHECK(effect[1].stale);

    // The same verb with other operands differs; with the same, it does not.
    const auto d = ok(tools::diff(cfg, "dlc", "agent"));
    REQUIRE(d.size() == 2);
    CHECK(d[0].op_a == Symbol("#append"));
    CHECK(d[0].op_b == Symbol("#append"));
    CHECK(to_string(*d[0].a) == "kick");
    CHECK(d[1].op_b.empty());

    // explain: the list's own layer wins, each verb composes, located at its row.
    const auto entries = ok(tools::explain(cfg, Path(cancels)));
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].standing == tools::Standing::Wins);
    CHECK(entries[1].standing == tools::Standing::Composes);
    CHECK(entries[2].layer == Symbol("agent"));
    CHECK(entries[2].standing == tools::Standing::Composes);
    CHECK(entries[2].op == Symbol("#append"));
    CHECK(to_string(entries[2].values) == "palm");
    CHECK(entries[2].source.line == 1);
    // An element of the composed list that only a verb made: each verb says what it adds.
    const auto element = ok(tools::explain(cfg, Path("fighters.kestrel.moves.jab.cancels.3")));
    REQUIRE(element.size() == 2);
    CHECK(element[1].standing == tools::Standing::Composes);
    CHECK(to_string(element[1].values) == "palm");
    CHECK(ok(cfg.source(cancels)).layer == Symbol("default"));
}

TEST_CASE("the layer tools list a layer's declarations: effect, diff and explain") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    cfg.session().set_pack_levels({"game", "mod"});
    REQUIRE(cfg.push_layer("base", dir.write("base.dta",
                                             "#pack (core (level game))\n"
                                             "#shape (unit (hp int (@range 1 10)) (name symbol))\n"
                                             "(unit (hp 5) (name a))\n")));
    REQUIRE(cfg.push_layer("mod", dir.write("mod.dta",
                                            "#shape (unit (hp int (@range 1 99)))\n"
                                            "(unit (hp 50))\n")));
    // The mod's effect: its value, and its #shape over the base's, which an agent would
    // otherwise not see widen the range.
    const auto effect = ok(tools::layer_effect(cfg, "mod"));
    REQUIRE(effect.size() == 2);
    CHECK(effect[1].declares == Symbol("#shape"));
    CHECK(effect[1].path == Symbol("unit"));
    CHECK(to_string(effect[1].values) == "(unit (hp int (@range 1 99)))");
    REQUIRE(effect[1].below);
    CHECK(to_string(*effect[1].below) == "(unit (hp int (@range 1 10)) (name symbol))");
    CHECK(effect[1].below_source->layer == Symbol("base"));
    CHECK(effect[1].source.line == 1);
    const auto base = ok(tools::layer_effect(cfg, "base"));
    CHECK(std::count_if(base.begin(), base.end(), [](const LayerEffectEntry& e) {
              return e.declares == Symbol("#pack") && e.path == Symbol("core");
          }) == 1);
    // diff: the two layers' shapes differ; a pack only the base declares.
    const auto d = ok(tools::diff(cfg, "base", "mod"));
    CHECK(std::count_if(d.begin(), d.end(), [](const tools::DiffEntry& e) {
              return e.declares == Symbol("#shape") && e.a && e.b;
          }) == 1);
    CHECK(std::count_if(d.begin(), d.end(), [](const tools::DiffEntry& e) {
              return e.declares == Symbol("#pack") && e.a && !e.b;
          }) == 1);
    // explain: each layer that declares the shape, or its field.
    const auto shape = ok(tools::explain(cfg, Path("#shape.unit")));
    REQUIRE(shape.size() == 2);
    CHECK(shape[0].layer == Symbol("base"));
    CHECK(shape[1].layer == Symbol("mod"));
    const auto field = ok(tools::explain(cfg, Path("#shape.unit.name")));
    REQUIRE(field.size() == 1);
    CHECK(to_string(field[0].values) == "(name symbol)");
    CHECK_FALSE(tools::explain(cfg, Path("#shape.nothing")));
}

TEST_CASE("a row's values come one node each, a verb's list below as the list it changes") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default",
                           dir.write("default.dta",
                                     "(aim 1 1)\n(pair (1 1))\n"
                                     "(moves\n   (jab (startup 4))\n   (heavy (startup 12)))\n")));
    REQUIRE(cfg.push_layer("agent", dir.write("agent.dta",
                                              "(aim 0 0)\n(pair (0 0))\n"
                                              "(moves #append (jab2 (startup 3)))\n")));
    const auto effect = ok(tools::layer_effect(cfg, "agent"));
    REQUIRE(effect.size() == 3);
    auto entry = [&](const char* path) {
        for (const LayerEffectEntry& e : effect)
            if (e.path == Symbol(path)) return &e;
        FAIL("no entry for " << path);
        return static_cast<const LayerEffectEntry*>(nullptr);
    };
    // Two values, and one value that is an array: told apart.
    const LayerEffectEntry* aim = entry("aim");
    CHECK(aim->values.size() == 2);
    CHECK(aim->below->size() == 2);
    const LayerEffectEntry* pair = entry("pair");
    REQUIRE(pair->values.size() == 1);
    CHECK(to_string(pair->values[0]) == "(0 0)");
    REQUIRE(pair->below->size() == 1);
    CHECK(to_string((*pair->below)[0]) == "(1 1)");
    // A record appended is one operand; below is the list of records it appends to, with the
    // source of the row that wrote it.
    const LayerEffectEntry* moves = entry("moves");
    CHECK(moves->op == Symbol("#append"));
    REQUIRE(moves->values.size() == 1);
    CHECK(to_string(moves->values[0]) == "(jab2 (startup 3))");
    REQUIRE(moves->below);
    REQUIRE(moves->below->size() == 2);
    CHECK(to_string(*moves->below) == "(jab (startup 4)) (heavy (startup 12))");
    REQUIRE(moves->below_source);
    CHECK(moves->below_source->layer == Symbol("default"));
    CHECK(moves->below_source->line == 3);
    // The merged list is the list below with the operand after it.
    CHECK(cfg.root()->try_get("moves")->values().size() == 3);

    // The same from a check of a candidate, and for a #drop.
    const auto report = ok(cfg.check_layer(
        "candidate", parse("(moves #drop (heavy (startup 12)))\n(pair (2 2))\n"), {}));
    REQUIRE(report.effect.size() == 2);
    const LayerEffectEntry& drop = report.effect[0];
    CHECK(drop.op == Symbol("#drop"));
    REQUIRE(drop.below);
    CHECK(drop.below->size() == 3);  // the stack's list, the agent's jab2 in it
    REQUIRE(drop.below_source);
    CHECK(drop.below_source->layer == Symbol("default"));
    CHECK(report.effect[1].values.size() == 1);

    // explain and diff give values one node each too.
    const auto explained = ok(tools::explain(cfg, Path("pair")));
    CHECK(explained[explained.size() - 1].values.size() == 1);
    const auto d = ok(tools::diff(cfg, "default", "agent"));
    for (const tools::DiffEntry& x : d)
        if (x.path == Symbol("aim")) {
            CHECK(x.a->size() == 2);
            CHECK(x.b->size() == 2);
        }
}

TEST_CASE("a pin and a verb's warnings read a list of records as the list below") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer(
        "default", dir.write("default.dta", "(moves (jab (startup 4)) (heavy (startup 12)))\n")));
    // A pin naming the list below holds; the record already there is reported.
    const auto r = ok(cfg.check_layer(
        "candidate",
        parse("(moves #append (jab (startup 4)) (@was (jab (startup 4)) (heavy (startup 12))))\n"),
        {}));
    REQUIRE(r.effect.size() == 1);
    CHECK_FALSE(r.effect[0].stale);
    bool held = false;
    for (const Error& w : r.warnings)
        held = held || w.message().find("already") != std::string::npos;
    CHECK(held);
}

// The warning and the folded counts are for the tools: a shipping build compiles them out.
BAKER_DEV_TEST_CASE(
    "a layer over a list of same-tagged records: the check warns, and effect "
    "and diff say what folded") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer(
        "default", dir.write("default.dta",
                             "(opening (build infantry) (build infantry) (build tank))\n"
                             "(units (a 1) (b 2))\n(items (sword 1))\n(order infantry tank)\n")));
    // Alone, the bottom layer keeps its list as written.
    CHECK(to_string(*cfg.root()->try_get("opening")) ==
          "(opening (build infantry) (build infantry) (build tank))");

    // A layer that restates the list in a new order: the check says it folds.
    const char* reorder = "(opening (build tank) (build infantry) (build infantry))\n";
    const auto r = ok(cfg.check_layer("agent", parse(reorder), {}));
    REQUIRE(r.warnings.size() == 1);
    const Error& w = r.warnings[0];
    CHECK(std::string(w.message()).find("'opening' below holds 3 (build ...)") == 0);
    CHECK(std::string(w.message()).find("#append") != std::string::npos);
    CHECK(w.path() == "opening.build");
    CHECK(w.line() == 1);
    REQUIRE(r.effect.size() == 1);
    CHECK(r.effect[0].folded == 2);

    // Records with values the layer itself writes twice, where the key exists below.
    const auto twice = ok(cfg.check_layer("agent", parse("(units (a 3) (a 4))\n"), {}));
    REQUIRE(twice.warnings.size() == 1);
    CHECK(std::string(twice.warnings[0].message()).find("the layer writes 2 (a ...) in 'units'") ==
          0);

    // Quiet: a plain list restated, a verb on it, a table split across rows, a new key, a
    // record set once.
    for (const char* quiet : {"(order tank infantry)\n", "(order #append cavalry)\n",
                              "(items (bow 2))\n(items (axe 3))\n", "(waves (wave 1) (wave 2))\n",
                              "(units (a 5))\n", "(opening #append (build cavalry))\n"}) {
        CAPTURE(quiet);
        CHECK(ok(cfg.check_layer("agent", parse(quiet), {})).warnings.empty());
    }

    // Pushed, the stack reads the old list; effect and diff say the layer's rows folded.
    REQUIRE(cfg.push_layer("agent", dir.write("agent.dta", reorder)));
    CHECK(to_string(*cfg.root()->try_get("opening")) ==
          "(opening (build infantry) (build infantry) (build tank))");
    const auto effect = ok(tools::layer_effect(cfg, "agent"));
    REQUIRE(effect.size() == 1);
    CHECK(effect[0].folded == 2);
    // The first rows match (tank against infantry differs anyway); a layer whose first row is
    // the same but whose folded rows differ is a difference too.
    REQUIRE(cfg.push_layer("same_first",
                           dir.write("same_first.dta", "(opening (build tank) (build tank))\n")));
    const auto d = ok(tools::diff(cfg, "agent", "same_first"));
    REQUIRE(d.size() == 1);
    CHECK(d[0].folded_a == 2);
    CHECK(d[0].folded_b == 1);
}
