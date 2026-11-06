// baker::shape: declarations, checking, enumeration, and the Config hook.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/shape.hpp>
#include <baker/std_interop.hpp>

#include "alloc_counter.hpp"
#include "ok.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace baker;
using baker::test::ok;

namespace {

const std::string kDir = BAKER_SHAPE_DIR;

std::vector<std::string> texts(const List<Error>& list) {
    std::vector<std::string> out;
    for (const Error& e : list) out.emplace_back(to_string(e));
    return out;
}

// The shapes `text` declares, added to `set`; its tree.
Ref<Array> add_text(shape::Set& set, std::string_view text, shape::Report& report,
                    std::string_view name = "t.dta") {
    LoadSession session;
    auto tree = ok(read_string(text, name, session));
    set.add(session, report);
    return tree;
}
Ref<Array> add_text(shape::Set& set, std::string_view text, std::string_view name,
                    shape::Report& report) {
    return add_text(set, text, report, name);
}

shape::Report check_text(std::string_view text) {
    shape::Set set;
    shape::Report report;
    const Ref<Array> tree = add_text(set, text, report);
    shape::check(*tree, set, report);
    return report;
}

}  // namespace

TEST_CASE("a violation in corpus-style data is reported at load, located") {
    Config cfg({.tweaks = nullptr});
    std::vector<std::string> reported;
    cfg.set_warning_handler([&](const Error& e) { reported.emplace_back(to_string(e)); });
    cfg.add_validator(shape::validator(), {.any_thread = true});
    REQUIRE(cfg.push_layer("bosses", kDir + "/bosses.dta"));
    // Before the patch: the dragon's third phase is out of range, the golem's phase lacks
    // its cadence (errors), and the golem's taunts is not declared (a warning).
    const auto has = [&](const std::string& part) {
        return std::any_of(reported.begin(), reported.end(),
                           [&](const std::string& r) { return r.find(part) != std::string::npos; });
    };
    CHECK(reported.size() == 3);
    CHECK(has("'enter_at' is 1.5, outside 0.0 to 1.0  (" + kDir +
              "/bosses.dta:22, at "
              "bosses/boss/phases/boss_phase/enter_at)"));
    CHECK(has("'boss_phase' has no 'cadence'  (" + kDir + "/bosses.dta:29"));
    CHECK(has("'taunts' is not a field of shape 'boss'"));
    // The tutorial's boss_phase is outside (at bosses boss phases): not checked.
    CHECK_FALSE(has("tutorial"));

    // The patch layer retunes the range and declares taunts: one error remains.
    reported.clear();
    REQUIRE(cfg.push_layer("patch", kDir + "/bosses_patch.dta"));
    CHECK(reported.size() == 1);
    CHECK(has("has no 'cadence'"));
}

TEST_CASE("types, lists, nested shapes, and what is not checked") {
    const auto r = check_text(R"(
        #shape (vec (x float) (y float)) #shape (unit (name string) (alive bool) (speed float (@range 0 10)) (tags (list symbol))
                (pos vec) (path (list vec)) (data any) (count int))
        (unit (name "bob") (alive 1) (speed {compute}) (tags a b 3) (pos (x 1) (y "no"))
              (path ((x 1) (y 2)) (p2 (x 1))) (data whatever 1 2) (count 2.5) (extra 1) 7)
    )");
    const auto e = texts(r.errors);
    const auto w = texts(r.warnings);
    auto has = [](const std::vector<std::string>& list, const std::string& part) {
        return std::any_of(list.begin(), list.end(),
                           [&](const std::string& s) { return s.find(part) != std::string::npos; });
    };
    CHECK(has(e, "'tags' is int, not symbol"));  // a list's element
    CHECK(has(e, "'y' is string, not float"));   // inside a nested shape
    CHECK(has(e, "'vec' has no 'y'"));           // in a list of shapes, the second
    CHECK(has(e, "'count' is float, not int"));
    CHECK(e.size() == 4);  // {compute} and `any` pass
    CHECK(has(w, "'extra' is not a field of shape 'unit'"));
    CHECK(has(w, "a value that is not a field of shape 'unit'"));
    CHECK(w.size() == 2);
}

TEST_CASE("a nested shape tagged with its own name is checked once") {
    const auto r = check_text(R"(
        #shape (attack (dmg int (@range 0 10))) #shape (boss (attacks (list attack)) (main attack))
        (boss (attacks (attack (dmg 50)) (attack (dmg 1))) (main (dmg 11)))
        (attack (dmg 12))
    )");
    CHECK(texts(r.errors).size() == 3);  // 50, 11, and the loose attack's 12
}

TEST_CASE("checking allocates the same for 10 instances as for 1,000") {
    auto allocations_for = [](int units) {
        std::string text = "#shape (vec (x float) (y float)) #shape (unit (hp int) (pos vec))";
        for (int i = 0; i < units; ++i) text += "(unit (hp 1) (pos (x 1) (y 2)))";
        shape::Set set;
        shape::Report report;
        const Ref<Array> tree = add_text(set, text, report);
        const test::AllocationScope scope;
        shape::check(*tree, set, report);
        return scope.count();
    };
    if (!test::counting_allocations()) return;
    CHECK(allocations_for(1000) == allocations_for(10));
}

TEST_CASE("malformed declarations are located errors; unknown shapes are reported") {
    const auto r = check_text(R"(#shape (s (a) (b float (@range 2 1)) (c (list)) 5) 7
                                  (s (c 1) (d 2)) #shape (t (u nowhere)) (t (u 1)))");
    const auto e = texts(r.errors);
    CHECK(e.size() == 5);
    CHECK(std::count_if(e.begin(), e.end(), [](const std::string& s) {
              return s.find("t.dta:1)") != std::string::npos;
          }) == 4);
    CHECK(std::any_of(e.begin(), e.end(), [](const std::string& s) {
        return s.find("'u' is of the unknown shape 'nowhere'") != std::string::npos;
    }));
}

TEST_CASE("a shape is declared whole: a layer verb in #shape is an error") {
    const auto r =
        check_text("#shape (fighter (health int)) #shape (move (cancels #append symbol))");
    const auto e = texts(r.errors);
    REQUIRE(e.size() == 1);
    CHECK(e[0].find("shape 'move' holds (cancels #append symbol): a shape is declared whole") !=
          std::string::npos);
}

TEST_CASE("the grammar's words carry the sigil, so a field may have any name") {
    // A shape describing itself, and fields named at, kind, desc and rule.
    const auto good = check_text(R"(#shape (piece (@at pieces) (@keyed) (@desc "A part")
                                       (@at symbol) (kind symbol (@one_of ramp turn))
                                       (desc string (@optional)) (rule int (@range 0 9)))
                                     (pieces (p1 (at a) (kind ramp) (rule 3))))");
    CHECK(good.errors.empty());
    const auto bad =
        check_text(R"(#shape (s (@rnage 0 1) (a int (range 0 1)) (b int (@rnage 0 1))))");
    const auto e = texts(bad.errors);
    CHECK(std::any_of(e.begin(), e.end(), [](const std::string& s) {
        return s.find("shape 's' has unknown metadata '@rnage'") != std::string::npos;
    }));
    CHECK(std::any_of(e.begin(), e.end(), [](const std::string& s) {
        return s.find("which is not metadata: write (@range ...)") != std::string::npos;
    }));
    CHECK(std::any_of(e.begin(), e.end(), [](const std::string& s) {
        return s.find("field 'b' has unknown metadata '@rnage'") != std::string::npos;
    }));
}

TEST_CASE("shapes are enumerable, with their metadata, for tuning panels") {
    LoadSession session;
    auto tree = ok(read_file(kDir + "/bosses.dta", session));
    auto patch = ok(read_file(kDir + "/bosses_patch.dta", session));
    shape::Set set;
    shape::Report report;
    set.add(session, report);
    CHECK(report.ok());
    REQUIRE(set.shapes().size() == 2);
    const shape::Shape* phase = set.find("boss_phase");
    REQUIRE(phase);
    REQUIRE(phase->at.size() == 3);
    CHECK(phase->at[1] == Symbol("boss"));
    REQUIRE(phase->fields.size() == 4);
    const shape::Field* enter = phase->field("enter_at");
    REQUIRE(enter);
    CHECK(enter->type == shape::Type::Float);
    CHECK(enter->max == 2.0);  // the patch's range
    CHECK(enter->kind == Symbol("percent"));
    CHECK(enter->desc == "HP fraction that starts this phase");
    CHECK(phase->field("attacks")->type == shape::Type::List);
    CHECK(phase->field("attacks")->element == shape::Type::Symbol);
    CHECK(phase->field("music")->optional);
    CHECK(set.find("boss")->field("taunts")->optional);
    // A copy stands alone.
    shape::Set copy = set;
    set = shape::Set();
    CHECK(copy.find("boss_phase")->field("enter_at")->desc == "HP fraction that starts this phase");
}

TEST_CASE("a (keyed) shape governs every child at its path, whatever its tag") {
    const auto r = check_text(R"(
        #shape (move (@at fighters * moves) (@keyed)
             (startup int (@range 1 60))
             (cancels (list symbol) (@optional)))
        (fighters
          (kestrel (moves
             (jab (startup 4) (cancels jab))
             (heavy (startup 900) (armor 1))
             (palm (cancels jab))))
          (wren (moves (poke (startup 3)))))
        (moves (stray (startup 900)))   ; not under fighters/*/moves
    )");
    const auto e = texts(r.errors);
    const auto w = texts(r.warnings);
    REQUIRE(e.size() == 2);
    CHECK(e[0].find("'startup' is 900, outside 1.0 to 60.0") != std::string::npos);
    CHECK(e[0].find("fighters/kestrel/moves/heavy/startup") != std::string::npos);
    CHECK(e[1].find("'move' has no 'startup'") != std::string::npos);  // palm
    REQUIRE(w.size() == 1);
    CHECK(w[0].find("'armor' is not a field of shape 'move'") != std::string::npos);

    const auto bad = check_text("#shape (move (@keyed) (startup int))");
    REQUIRE(bad.errors.size() == 1);
    CHECK(texts(bad.errors)[0].find("no (@at ...)") != std::string::npos);
}

TEST_CASE("a softer pack may narrow a harder pack's shape, not loosen it") {
    static const std::map<std::string, std::string> files = {
        {"base.dta",
         "#pack (game_rules (level game))\n"
         "#shape (player (speed int (@range 1 10)) (name symbol))\n(player (speed 5) (name p))"},
        {"mod.dta",
         "#pack (fast_mod (level mod))\n"
         "#shape (player (speed int (@range 1 1000)) (name symbol (@optional)))\n"
         "(player (speed 900))"},
        {"tight.dta",
         "#pack (tight_mod (level mod))\n#shape (player (speed int (@range 2 8)))\n"
         "(player (speed 9))"},
        {"retype.dta", "#pack (retype_mod (level mod))\n#shape (player (speed float))"},
        {"loose_base.dta", "#shape (player (speed int (@range 1 10)))\n(player (speed 5))"},
        {"loose_mod.dta", "#shape (player (speed int (@range 1 1000)))\n(player (speed 900))"},
        {"buttons.dta",
         "#pack (game_rules (level game))\n"
         "#shape (move (button symbol (@one_of light heavy)))\n(move (button light))"},
        {"more_buttons.dta",
         "#pack (more_mod (level mod))\n#shape (move (button symbol (@one_of kick)))\n"
         "(move (button kick))"},
        {"counts.dta",
         "#pack (game_rules (level game))\n#shape (unit (pos (list float 3)))\n(unit (pos 1 2 "
         "3))"},
        {"wide_counts.dta",
         "#pack (wide_mod (level mod))\n#shape (unit (pos (list float 2 4)))\n(unit (pos 1 2))"}};
    struct Run {
        Config cfg{{.tweaks = nullptr}};
        std::vector<std::string> reported;
        explicit Run(std::initializer_list<const char*> layers) {
            cfg.session().set_reader([](std::string_view p, TextSink out) {
                const auto it = files.find(std::string(p));
                if (it == files.end()) return false;
                out(it->second);
                return true;
            });
            cfg.session().set_pack_levels({"game", "mod"});
            cfg.set_warning_handler([&](const Error& e) { reported.emplace_back(to_string(e)); });
            cfg.add_validator(shape::validator(), {.any_thread = true});
            for (const char* l : layers) REQUIRE(cfg.push_layer(l, l));
        }
        bool has(const std::string& part) const {
            return std::any_of(reported.begin(), reported.end(), [&](const std::string& r) {
                return r.find(part) != std::string::npos;
            });
        }
    };
    // Widening the range and making a required field optional are refused, and the base's
    // range still governs: 900 is out of range.
    Run widen{"base.dta", "mod.dta"};
    CHECK(widen.has("'speed' is declared by a harder pack"));
    CHECK(widen.has("'name' is declared by a harder pack"));
    CHECK(widen.has("'speed' is 900, outside 1.0 to 10.0"));
    // Narrowing is the softer pack's to do: 9 is outside its 2 to 8.
    Run narrow{"base.dta", "tight.dta"};
    CHECK_FALSE(narrow.has("declared by a harder pack"));
    CHECK(narrow.has("'speed' is 9, outside 2.0 to 8.0"));
    // Another type is a loosening too.
    Run retype{"base.dta", "retype.dta"};
    CHECK(retype.has("'speed' is declared by a harder pack"));
    // Adding a symbol to a harder pack's (one_of ...) loosens it too.
    Run buttons{"buttons.dta", "more_buttons.dta"};
    CHECK(buttons.has("'button' is declared by a harder pack"));
    CHECK(buttons.has("'button' is 'kick', not one of light, heavy"));
    // Widening a list's count loosens it, and the harder count holds.
    Run counts{"counts.dta", "wide_counts.dta"};
    CHECK(counts.has("'pos' is declared by a harder pack"));
    CHECK(counts.has("'pos' holds 2 values, not 3"));
    // Files that declare no pack are unconstrained, as before.
    Run loose{"loose_base.dta", "loose_mod.dta"};
    CHECK(loose.reported.empty());
}

TEST_CASE("a symbol field lists the symbols it allows, (one_of ...)") {
    const char* const kShapes =
        "#shape (move (button symbol (@one_of light heavy kick special))"
        " (combo (list symbol) (@one_of light heavy)) (startup int (@optional)))\n";
    const shape::Report bad =
        check_text(std::string(kShapes) + "(move (button lite) (combo light heavy kick))\n");
    CHECK(texts(bad.errors) ==
          std::vector<std::string>{
              "baker: 'button' is 'lite', not one of light, heavy, kick, special  (t.dta:2, at "
              "move/button)",
              "baker: 'combo' is 'kick', not one of light, heavy  (t.dta:2, at move/combo)"});
    CHECK(check_text(std::string(kShapes) + "(move (button kick) (combo heavy light))\n").ok());
    CHECK(check_text(std::string(kShapes) + "(move (button {pick}) (combo light))\n")
              .ok());  // known only when run

    // Declared again, a field adds what the new list names, in the order written.
    shape::Set set;
    shape::Report report;
    add_text(set, "#shape (move (button symbol (@one_of light heavy)))", "a.dta", report);
    add_text(set, "#shape (move (button symbol (@one_of special light)))", "b.dta", report);
    CHECK(report.ok());
    const shape::Field* button = set.find(Symbol("move"))->field(Symbol("button"));
    REQUIRE(button);
    std::vector<std::string> names;
    for (const Symbol s : button->one_of) names.emplace_back(s.str());
    CHECK(names == std::vector<std::string>{"light", "heavy", "special"});
    std::string json;
    shape::write_json(set, string_out(json));
    CHECK(json.find(R"j("one_of": ["light", "heavy", "special"])j") != std::string::npos);

    // A list on a field that is not a symbol, or holding something else, is an error.
    CHECK(texts(check_text("#shape (s (n int (@one_of a b)))").errors) ==
          std::vector<std::string>{
              "baker: field 'n' lists (@one_of ...) symbols, so its type is symbol or (list "
              "symbol)  (t.dta:1)"});
    CHECK_FALSE(check_text("#shape (s (n symbol (@one_of a 2)))").ok());
}

// --- As JSON, baker.shapes/1.8. ---

TEST_CASE("the shapes as JSON: each shape and its fields") {
    shape::Set shapes;
    shape::Report report;
    add_text(shapes,
             "#shape (boss_phase (@at bosses * phases) (@keyed)\n"
             "      (enter_at float (@range 0 1) (@step 0.05) (@desc \"HP fraction\"))\n"
             "      (adds (list symbol) (@optional)))\n",
             "shapes.dta", report);
    REQUIRE(report.errors.empty());
    std::string json;
    shape::write_json(shapes, string_out(json));
    CHECK(json.rfind("{\n  \"schema\": \"baker.shapes/1.8\",\n  \"shapes\": [\n", 0) == 0);
    CHECK(json.find(
              R"j({"name": "boss_phase", "at": ["bosses", "*", "phases"], "keyed": true, )j"
              R"j("view": false, "cxx_name": "", "file": "shapes.dta", "line": 1, "fields": [)j") !=
          std::string::npos);
    CHECK(
        json.find(
            R"j({"name": "enter_at", "type": "float", "element": "", "count": null, "shape": "", "table": "", "asset": "", )j"
            R"j("optional": false, "range": [0.0, 1.0], "step": 0.05, "kind": "", )j"
            R"j("desc": "HP fraction", "note": "", "one_of": [], "view": true, "cxx": "", "cxx_name": "", )j"
            R"j("renamed_from": [], )j"
            R"j("file": "shapes.dta", "line": 2})j") != std::string::npos);
    CHECK(
        json.find(R"j({"name": "adds", "type": "list", "element": "symbol", "count": null, )j"
                  R"j("shape": "", "table": "", "asset": "", "optional": true, "range": null,)j") !=
        std::string::npos);
}

// --- Keys of a table, (key_of TABLE). ---

TEST_CASE("a key field names a record of its table; a typo is located, with the key meant") {
    static constexpr std::string_view kShapes =
        "#shape (unit (@at units) (@keyed) (weapon (key_of weapons))\n"
        "   (spares (list (key_of weapons)) (@optional)) (backup (key_of weapons) (@optional)))\n"
        "(weapons (laser (damage 3)) (cannon (damage 9)))\n";
    CHECK(check_text(std::string(kShapes) +
                     "(units (scout (weapon laser) (spares cannon laser)) (tank (weapon cannon)))")
              .ok());
    const auto r = check_text(std::string(kShapes) +
                              "(units (scout (weapon lazer)) (tank (weapon cannon) (spares rail "
                              "laser) (backup 3)))");
    CHECK(texts(r.errors) ==
          std::vector<std::string>{
              "baker: 'lazer' is not a key of weapons (laser?)  (t.dta:4, at units/scout/weapon)",
              "baker: 'rail' is not a key of weapons  (t.dta:4, at units/tank/spares)",
              "baker: 'backup' is int, not a key of weapons  (t.dta:4, at units/tank/backup)"});
    // A dotted path from the root, and a table that nothing encloses.
    CHECK(check_text("#shape (s (@at uses) (@keyed) (w (key_of items.weapons)))\n"
                     "(items (weapons (laser)))\n(uses (a (w laser)))")
              .ok());
    CHECK(texts(check_text("#shape (s (@at uses) (@keyed) (w (key_of gear)))\n(uses (a (w x)))")
                    .errors) ==
          std::vector<std::string>{"baker: 'w' names a key of gear, and no table gear encloses it  "
                                   "(t.dta:2, at uses/a/w)"});
    // A path resolves by its first segment: the nearest `items` is the shop's, which has no
    // weapons, so the root's items.weapons is not reached.
    CHECK(texts(check_text("#shape (s (@at shop uses) (@keyed) (w (key_of items.weapons)))\n"
                           "(items (weapons (laser)))\n(shop (items (food)) (uses (a (w laser))))")
                    .errors) ==
          std::vector<std::string>{"baker: 'w' names a key of items.weapons, and no table "
                                   "items.weapons encloses it  (t.dta:3, at shop/uses/a/w)"});
    CHECK_FALSE(check_text("#shape (s (w (key_of)))").ok());
    CHECK_FALSE(check_text("#shape (s (w (key_of 3)))").ok());
}

TEST_CASE("a key resolves in the nearest enclosing table: each fighter's own moves") {
    static constexpr std::string_view kShapes =
        "#shape (move (@at fighters * moves) (@keyed) (cancel_into (list (key_of moves)) "
        "(@optional)))\n";
    CHECK(check_text(std::string(kShapes) +
                     "(fighters (ryu (moves (jab) (hadoken (cancel_into jab))))\n"
                     "   (ken (moves (kick) (shoryu (cancel_into kick)))))")
              .ok());
    // Ken's move may not cancel into Ryu's jab: Ken's moves are the nearest table.
    const auto r = check_text(std::string(kShapes) +
                              "(fighters (ryu (moves (jab)))\n"
                              "   (ken (moves (kick) (shoryu (cancel_into jab)))))");
    CHECK(texts(r.errors) ==
          std::vector<std::string>{"baker: 'jab' is not a key of moves  (t.dta:3, at "
                                   "fighters/ken/moves/shoryu/cancel_into)"});
}

TEST_CASE("keys follow the layers; a key across the one-way pack rule is a warning") {
    static const std::map<std::string, std::string> files = {
        {"base.dta",
         "#pack (game_rules (level game))\n"
         "#shape (unit (@at units) (@keyed) (weapon (key_of weapons)))\n"
         "(weapons (laser) (cannon))\n(units (scout (weapon laser)))"},
        {"add.dta", "(weapons (rail))\n(units (tank (weapon rail)))"},
        {"remove.dta", "(weapons (laser #remove))"},
        {"mod.dta", "#pack (mod_weapons (level mod))\n(weapons (plasma))"},
        {"uses_mod.dta", "#pack (game_rules (level game))\n(units (scout (weapon plasma)))"}};
    struct Run {
        Config cfg{{.tweaks = nullptr}};
        std::vector<std::string> reported;
        explicit Run(std::initializer_list<const char*> layers) {
            cfg.session().set_reader([](std::string_view p, TextSink out) {
                const auto it = files.find(std::string(p));
                if (it == files.end()) return false;
                out(it->second);
                return true;
            });
            cfg.session().set_pack_levels({"game", "mod"});
            cfg.set_warning_handler([&](const Error& e) { reported.emplace_back(to_string(e)); });
            cfg.add_validator(shape::validator(), {.any_thread = true});
            for (const char* l : layers) REQUIRE(cfg.push_layer(l, l));
        }
        bool has(const std::string& part) const {
            return std::any_of(reported.begin(), reported.end(), [&](const std::string& r) {
                return r.find(part) != std::string::npos;
            });
        }
    };
    // A layer's new key is a key; a key a layer removes is an error at the record naming it.
    CHECK(Run{"base.dta", "add.dta"}.reported.empty());
    Run removed{"base.dta", "remove.dta"};
    CHECK(
        removed.has("baker: 'laser' is not a key of weapons  (base.dta:4, at units/scout/weapon)"));
    // The game's own unit naming the mod's weapon crosses the one-way rule.
    Run crossed{"base.dta", "mod.dta", "uses_mod.dta"};
    CHECK(crossed.reported.size() == 1);
    CHECK(
        crossed.has("pack 'game_rules' (level game) names key 'plasma' of weapons from the "
                    "softer pack 'mod_weapons' (level mod)"));
}

TEST_CASE("a key field in the shapes JSON names its table") {
    shape::Set shapes;
    shape::Report report;
    add_text(shapes,
             "#shape (unit (weapon (key_of items.weapons))\n"
             "   (spares (list (key_of weapons))))",
             "shapes.dta", report);
    REQUIRE(report.errors.empty());
    std::string json;
    shape::write_json(shapes, string_out(json));
    CHECK(json.find(
              R"j({"name": "weapon", "type": "key", "element": "", "count": null, "shape": "", )j"
              R"j("table": "items.weapons", "asset": "", )j") != std::string::npos);
    CHECK(
        json.find(
            R"j({"name": "spares", "type": "list", "element": "key", "count": null, "shape": "", )j"
            R"j("table": "weapons", "asset": "", )j") != std::string::npos);
}

// --- Assets: (asset KIND) and the project's kinds. ---

TEST_CASE("an asset field holds a path of its kind; the uses are listed with their records") {
    shape::Set set;
    shape::Report report;
    add_text(set,
             "#shape (boss (@at bosses) (@keyed) (mesh (asset mesh)) (skins (list (asset texture)) "
             "(@optional)) (sound (asset sound) (@optional)))\n",
             "shapes.dta", report);
    REQUIRE(report.errors.empty());
    const shape::Shape* boss = set.find(Symbol("boss"));
    REQUIRE(boss);
    CHECK(boss->fields[0].type == shape::Type::Asset);
    CHECK(boss->fields[0].asset == Symbol("mesh"));
    CHECK(boss->fields[1].type == shape::Type::List);
    CHECK(boss->fields[1].element == shape::Type::Asset);
    CHECK(boss->fields[1].asset == Symbol("texture"));
    const Symbol glb(".glb"), fbx(".fbx"), png(".png");
    const Symbol mesh_ext[] = {glb, fbx};
    const Symbol tex_ext[] = {png};
    const shape::AssetKind kinds[] = {{Symbol("mesh"), List<Symbol>(mesh_ext)},
                                      {Symbol("texture"), List<Symbol>(tex_ext)},
                                      {Symbol("any"), {}}};
    set.add_asset_kinds(kinds);
    REQUIRE(set.asset_kinds().size() == 3);
    CHECK(set.asset_kind(Symbol("mesh"))->extensions.size() == 2);

    auto tree = ok(read_string(
        "(bosses (dragon (@id a0000000000000000000000000000001) (mesh \"art/dragon.glb\")"
        " (skins \"art/red.png\" \"art/blue.tga\"))"
        " (golem (mesh \"art/golem.obj\") (sound \"snd/golem.wav\"))"
        " (wisp (mesh 3)))",
        "data.dta"));
    shape::Report r;
    shape::check(*tree, set, r);
    const auto e = texts(r.errors);
    const auto has = [&](std::string_view part) {
        return std::any_of(e.begin(), e.end(),
                           [&](const std::string& x) { return x.find(part) != std::string::npos; });
    };
    CHECK(has("'skins' is \"art/blue.tga\", not a texture (.png)"));
    CHECK(has("'mesh' is \"art/golem.obj\", not a mesh (.glb or .fbx)"));
    CHECK(has("'sound' is an (asset sound), and the project declares no kind 'sound'"));
    CHECK(has("'mesh' is int, not asset"));
    CHECK(e.size() == 4);
    // The uses, every one, with the record's path and ID.
    const auto uses = shape::asset_uses(*tree, set);
    REQUIRE(uses.size() == 5);
    CHECK(uses[0].target == "art/dragon.glb");
    CHECK(uses[0].kind == Symbol("mesh"));
    CHECK(uses[0].field == Symbol("mesh"));
    CHECK(uses[0].path == Symbol("bosses/dragon/mesh"));
    CHECK(uses[0].id == *RecordId::parse("a0000000000000000000000000000001"));
    CHECK(uses[0].line == 1);
    CHECK(uses[2].target == "art/blue.tga");
    CHECK(uses[3].id.empty());
    // The digest spells the kind; the JSON carries it.
    std::string json;
    shape::write_json(set, string_out(json));
    CHECK(
        json.find(
            R"j("type": "asset", "element": "", "count": null, "shape": "", "table": "", "asset": "mesh")j") !=
        std::string::npos);
    CHECK(
        json.find(
            R"j("type": "list", "element": "asset", "count": null, "shape": "", "table": "", "asset": "texture")j") !=
        std::string::npos);
    // The declaration's errors.
    shape::Set bad;
    shape::Report rep;
    add_text(bad, "#shape (s (x (asset)))", rep);
    CHECK(!rep.errors.empty());
}

// --- Rules, read here and evaluated by baker::tools. ---

TEST_CASE("a shape's rules are read, replaced by message, and malformed ones located") {
    shape::Set shapes;
    shape::Report report;
    add_text(shapes,
             "#shape (unit (@at units) (@keyed) (max_hp int) (start_hp int)\n"
             "   (@rule \"starts at or below its maximum\" {<= $start_hp $max_hp})\n"
             "   (@rule \"has some health\" {> $max_hp 0} (@warn)))\n",
             "base.dta", report);
    REQUIRE(report.errors.empty());
    const shape::Shape* unit = shapes.find(Symbol("unit"));
    REQUIRE(unit);
    REQUIRE(unit->rules.size() == 2);
    CHECK(unit->rules[0].message == "starts at or below its maximum");
    CHECK_FALSE(unit->rules[0].warn);
    CHECK(unit->rules[1].warn);
    CHECK(unit->rules[0].line == 2);
    // A later layer softens the first and adds a third.
    add_text(shapes,
             "#shape (unit\n"
             "   (@rule \"starts at or below its maximum\" {<= $start_hp "
             "$max_hp} (@warn))\n"
             "   (@rule \"is named\" {!= $name 0}))\n",
             "mod.dta", report);
    unit = shapes.find(Symbol("unit"));
    REQUIRE(unit->rules.size() == 3);
    CHECK(unit->rules[0].warn);
    CHECK(unit->rules[0].file == Symbol("mod.dta"));

    CHECK(texts(check_text("#shape (s (@rule) (@rule \"m\") (@rule \"m\" 3) (@rule \"m\" {f} x))")
                    .errors) ==
          std::vector<std::string>{
              "baker: a rule is declared as (@rule \"message\" {expression}), optionally (@warn)  "
              "(t.dta:1)",
              "baker: a rule is declared as (@rule \"message\" {expression}), optionally (@warn)  "
              "(t.dta:1)",
              "baker: rule \"m\" holds 3, not an {expression}  (t.dta:1)",
              "baker: rule \"m\" has x after its expression; only (@warn) may follow  (t.dta:1)"});
}

TEST_CASE("a softer pack may make a harder pack's rule an error, not soften or change it") {
    static const std::map<std::string, std::string> files = {
        {"base.dta",
         "#pack (game_rules (level game))\n"
         "#shape (unit (@rule \"positive\" {> $hp 0}))\n"},
        {"soften.dta",
         "#pack (mod (level mod))\n#shape (unit (@rule \"positive\" {> $hp 0} (@warn)))\n"},
        {"change.dta", "#pack (mod (level mod))\n#shape (unit (@rule \"positive\" {> $hp -5}))\n"},
        {"warned.dta",
         "#pack (game_rules (level game))\n"
         "#shape (unit (@rule \"positive\" {> $hp 0} (@warn)))\n"},
        {"harden.dta", "#pack (mod (level mod))\n#shape (unit (@rule \"positive\" {> $hp 0}))\n"}};
    LoadSession session;
    session.set_reader([](std::string_view p, TextSink out) {
        const auto it = files.find(std::string(p));
        if (it == files.end()) return false;
        out(it->second);
        return true;
    });
    session.set_pack_levels({"game", "mod"});
    auto layered = [&](const char* below, const char* above) {
        LoadSession both(session);  // the two files' declarations alone
        shape::Set shapes;
        shape::Report report;
        ok(read_file(below, both));
        ok(read_file(above, both));
        shapes.add(both, report);
        return std::make_pair(shapes.find(Symbol("unit"))->rules[0], report);
    };
    auto [soft, r1] = layered("base.dta", "soften.dta");
    CHECK_FALSE(soft.warn);
    CHECK(texts(r1.warnings).size() == 1);
    auto [changed, r2] = layered("base.dta", "change.dta");
    CHECK(to_string(changed.expression) == "{> $hp 0}");
    CHECK(texts(r2.warnings).size() == 1);
    auto [hard, r3] = layered("warned.dta", "harden.dta");
    CHECK_FALSE(hard.warn);
    CHECK(r3.warnings.empty());
}

TEST_CASE("each_ruled visits the records a shape with rules governs, with their paths") {
    shape::Set shapes;
    shape::Report report;
    const Ref<Array> tree =
        add_text(shapes,
                 "#shape (unit (@at units) (@keyed) (@rule \"r\" {f}))\n"
                 "#shape (boss_phase (@rule \"p\" {g}))\n"
                 "#shape (plain (n int))\n"
                 "(units (grunt (hp 1)) (tank (hp 2)))\n"
                 "(bosses (dragon (phases (boss_phase (at 1)) (boss_phase (at 2)))))\n"
                 "(plain (n 3))\n",
                 report);
    std::vector<std::string> seen;
    shape::each_ruled(*tree, shapes,
                      [&](const Array& record, const shape::Shape& s, std::string_view path) {
                          seen.push_back(std::string(s.name.str()) + " " + std::string(path) +
                                         " :" + std::to_string(record.line()));
                      });
    CHECK(seen == std::vector<std::string>{"unit units/grunt :4", "unit units/tank :4",
                                           "boss_phase bosses/dragon/phases/boss_phase :5",
                                           "boss_phase bosses/dragon/phases/boss_phase :5"});
}

TEST_CASE("the shapes JSON lists each shape's rules") {
    shape::Set shapes;
    shape::Report report;
    add_text(shapes,
             "#shape (unit (hp int)\n"
             "   (@rule \"positive\" {> $hp 0} (@warn)))\n",
             "shapes.dta", report);
    std::string json;
    shape::write_json(shapes, string_out(json));
    CHECK(json.find(R"j(], "rules": [)j"
                    "\n"
                    R"j(      {"message": "positive", "expression": "{> $hp 0}", "warn": true, )j"
                    R"j("file": "shapes.dta", "line": 2})j") != std::string::npos);
}

TEST_CASE("a build without baker::script says once that rules went unchecked") {
    Config cfg({.tweaks = nullptr});
    std::vector<std::string> reported;
    cfg.set_warning_handler([&](const Error& e) { reported.emplace_back(to_string(e)); });
    cfg.add_validator(shape::validator(), {.any_thread = true});
    REQUIRE(cfg.push_layer_text("base", "#shape (unit (@rule \"r\" {f}))\n"));
    REQUIRE(cfg.push_layer("more", ok(read_string("(x 1)\n", "u.dta"))));
    const auto told = std::count_if(reported.begin(), reported.end(), [](const std::string& r) {
        return r.find("has no baker::script to check them") != std::string::npos;
    });
    CHECK(told == (BAKER_TEST_SCRIPT ? 0 : 1));
}

TEST_CASE("a curve field holds a curve, and its range each point's y") {
    const char* const shapes =
        "#shape (enemy (@at enemies) (@keyed)\n"
        "   (falloff curve (@range 0 1))\n"
        "   (pace curve))\n";
    CHECK(check_text(std::string(shapes) + "(enemies (grunt (falloff (0 1) (10 0.5) (20 0)) "
                                           "(pace catmull-rom loop (0 1) (60 3 0 0))))\n")
              .errors.empty());
    CHECK(texts(check_text(std::string(shapes) +
                           "(enemies\n (grunt (falloff (0 1) (10 0.5) (5 0)) (pace (0 1))))\n")
                    .errors) ==
          std::vector<std::string>{"baker: 'falloff' is not a curve: (5 0) is not after the "
                                   "point before it in x  (t.dta:5, at enemies/grunt/falloff)"});
    CHECK(texts(check_text(std::string(shapes) +
                           "(enemies\n (grunt (falloff (0 1) (10 2)) (pace wobbly (0 1))))\n")
                    .errors) ==
          std::vector<std::string>{
              "baker: 'falloff''s point (10 2) has y outside 0.0 to 1.0  (t.dta:5, at "
              "enemies/grunt/falloff)",
              "baker: 'pace' is not a curve: wobbly is not an interpolation (monotone, linear, "
              "catmull-rom, constant) or an extension (clamp, extrapolate, loop), or names one "
              "twice  (t.dta:5, at enemies/grunt/pace)"});
}

TEST_CASE("a list may give its count: (list T N) and (list T N M)") {
    const auto r = check_text(
        "#shape (unit (pos (list float 3)) (tags (list symbol 1 2) (@optional))"
        " (spares (list (key_of weapons) 0 1) (@optional)))\n"
        "(weapons (laser) (rifle))\n"
        "(unit (pos 1 2 3) (tags a b))\n(unit (pos 1 2) (tags))\n(unit (pos 1 2 3 4) (spares laser "
        "rifle))\n");
    const auto e = texts(r.errors);
    CHECK(e.size() == 4);
    CHECK(std::count_if(e.begin(), e.end(), [](const std::string& x) {
              return x.find("'pos' holds 2 values, not 3") != std::string::npos ||
                     x.find("'pos' holds 4 values, not 3") != std::string::npos ||
                     x.find("'tags' holds 0 values, not 1 to 2") != std::string::npos ||
                     x.find("'spares' holds 2 values, not 0 to 1") != std::string::npos;
          }) == 4);
    // A malformed count is a located error at the field.
    for (const char* bad : {"#shape (u (p (list float 3 1)))", "#shape (u (p (list float -1)))",
                            "#shape (u (p (list float x)))", "#shape (u (p (list float 1 2 3)))"}) {
        INFO(bad);
        CHECK(!check_text(bad).errors.empty());
    }
    // The JSON gives it as [min, max].
    shape::Set set;
    shape::Report report;
    add_text(set, "#shape (unit (pos (list float 3)) (tags (list symbol 1 2)))", "a.dta", report);
    std::string json;
    shape::write_json(set, string_out(json));
    CHECK(json.find(R"j("name": "pos", "type": "list", "element": "float", "count": [3, 3],)j") !=
          std::string::npos);
    CHECK(json.find(R"j("count": [1, 2])j") != std::string::npos);
}

TEST_CASE("a shape that governs nothing is named, where it is declared") {
    // A keyed shape one level off, (@at fighters moves) for (@at fighters * moves), checks
    // nothing, and check() says nothing of it: unused() names it.
    shape::Set set;
    shape::Report report;
    const Ref<Array> tree = add_text(
        set,
        "#shape (move (@at fighters moves) (@keyed) (startup int (@range 1 60)))\n"
        "#shape (fighter (@at fighters) (@keyed) (health int))\n"
        "#shape (boss (hp int))\n#shape (stats (speed float))\n#shape (hero (stats stats))\n"
        "(fighters (kestrel (health 3) (moves (jab (startup 900)))))\n(hero (stats (speed 1.5)))\n",
        report, "wrong.dta");
    shape::check(*tree, set, report);
    CHECK(report.errors.empty());  // the startup of 900 goes unchecked
    shape::Report found;
    shape::unused(*tree, set, found);
    const auto w = texts(found.warnings);
    REQUIRE(w.size() == 2);
    CHECK(w[0].find("shape 'move' governs nothing: no array is a child of fighters/moves") !=
          std::string::npos);
    CHECK(w[0].find("wrong.dta:1") != std::string::npos);
    CHECK(w[1].find("shape 'boss' governs nothing: no array is tagged 'boss'") !=
          std::string::npos);
}

// --- Renamed fields: (@renamed_from old). ---

TEST_CASE("a field spelled by a name its shape renamed is checked as the field, and listed") {
    shape::Set set;
    shape::Report report;
    add_text(set,
             "#shape (boss (@at bosses) (@keyed) (hp int (@range 1 100) (@renamed_from health "
             "hit_points))\n"
             "   (name string (@optional)))\n",
             "shapes.dta", report);
    REQUIRE(report.errors.empty());
    const shape::Shape* boss = set.find(Symbol("boss"));
    REQUIRE(boss);
    REQUIRE(boss->fields[0].renamed_from.size() == 2);
    CHECK(boss->renamed(Symbol("health")) == &boss->fields[0]);
    CHECK(boss->renamed(Symbol("hit_points")) == &boss->fields[0]);
    CHECK(boss->renamed(Symbol("hp")) == nullptr);

    auto tree =
        ok(read_string("(bosses (golem (health 40)) (dragon (hp 50)) (wisp (health 2) (hp 3))"
                       " (ghost (hit_points 400)))",
                       "data.dta"));
    shape::Report r;
    shape::check(*tree, set, r);
    // The golem's health is the field, warned; the wisp has both, an error; the ghost's
    // value is checked as hp, so its range holds.
    const auto w = texts(r.warnings);
    const auto e = texts(r.errors);
    REQUIRE(w.size() == 2);
    CHECK(w[0].find("'health' is now 'hp' (format rewrites it)") != std::string::npos);
    CHECK(w[1].find("'hit_points' is now 'hp'") != std::string::npos);
    REQUIRE(e.size() == 2);
    CHECK(e[0].find("'health' was renamed 'hp', and the record has both") != std::string::npos);
    const bool ranged = e[1].find("400") != std::string::npos;
    CHECK(ranged);  // the ghost's 400 is checked as hp, so its range holds
    // Each spelling, for format to rewrite, where the record spells it.
    REQUIRE(r.renames.size() == 3);
    CHECK(r.renames[0].old == Symbol("health"));
    CHECK(r.renames[0].name == Symbol("hp"));
    CHECK(r.renames[0].file == Symbol("data.dta"));
    CHECK(r.renames[0].line == 1);
    CHECK(r.renames[2].old == Symbol("hit_points"));
    // No "has no hp": the old spelling counts as the field.
    CHECK(std::none_of(e.begin(), e.end(),
                       [](const std::string& x) { return x.find("has no") != std::string::npos; }));

    // The declaration's own errors.
    const auto errors = [](std::string_view text) {
        shape::Set s;
        shape::Report rep;
        add_text(s, text, rep);
        return texts(rep.errors);
    };
    const auto has = [](const std::vector<std::string>& v, std::string_view part) {
        return std::any_of(v.begin(), v.end(),
                           [&](const std::string& x) { return x.find(part) != std::string::npos; });
    };
    CHECK(has(errors("#shape (s (x int (@renamed_from)))"), "names the field's earlier names"));
    CHECK(has(errors("#shape (s (x int (@renamed_from \"y\")))"), "holds field names, as symbols"));
    CHECK(has(errors("#shape (s (x int (@renamed_from x)))"), "its own name"));
    CHECK(has(errors("#shape (s (x int (@renamed_from y)) (y int))"),
              "'x' is (@renamed_from y), and 'y' is a field of the shape"));
    // Declared again, a field keeps its old names and adds the new list's.
    shape::Set again;
    shape::Report rep;
    add_text(again, "#shape (s (x int (@renamed_from a)))\n#shape (s (x int (@renamed_from b)))\n",
             rep);
    REQUIRE(rep.errors.empty());
    CHECK(again.find(Symbol("s"))->fields[0].renamed_from.size() == 2);
    // In the JSON.
    std::string json;
    shape::write_json(again, string_out(json));
    CHECK(json.find(R"j("renamed_from": ["a", "b"])j") != std::string::npos);
}

// --- Views: (@view), (@view no), (@cxx T), (@cxx_name N). ---

TEST_CASE(
    "a shape marked (@view) generates a view; its fields may opt out, pick a type, or a name") {
    shape::Set set;
    shape::Report report;
    add_text(set,
             "#shape (move (@at fighters * moves) (@keyed) (@view) (@cxx_name MoveView)\n"
             "   (startup int (@range 1 60) (@cxx uint16))\n"
             "   (damage float (@cxx double) (@cxx_name dmg))\n"
             "   (on_hit any (@view no))\n"
             "   (name symbol))\n"
             "#shape (plain (@at plains) (@keyed) (@view no) (x int))\n",
             report);
    REQUIRE(report.errors.empty());
    const shape::Shape* move = set.find("move");
    REQUIRE(move);
    CHECK(move->view);
    CHECK(move->cxx_name == Symbol("MoveView"));
    CHECK(move->field("startup")->cxx == Symbol("uint16"));
    CHECK(move->field("startup")->view);
    CHECK(move->field("damage")->cxx == Symbol("double"));
    CHECK(move->field("damage")->cxx_name == Symbol("dmg"));
    CHECK_FALSE(move->field("on_hit")->view);
    CHECK(move->field("name")->cxx.empty());  // the default
    CHECK_FALSE(set.find("plain")->view);
    // A shape declared again keeps its view flags unless it says otherwise.
    add_text(set, "#shape (move (damage float (@range 0 9)))", report, "patch.dta");
    REQUIRE(report.errors.empty());
    CHECK(set.find("move")->view);
    CHECK(set.find("move")->field("damage")->cxx == Symbol("double"));

    std::string json;
    shape::write_json(set, string_out(json));
    CHECK(json.find(R"j("keyed": true, "view": true, "cxx_name": "MoveView", "file")j") !=
          std::string::npos);
    CHECK(
        json.find(
            R"j("view": false, "cxx": "", "cxx_name": "", "renamed_from": [], "file": "t.dta", "line": 4})j") !=
        std::string::npos);
    CHECK(json.find(
              R"j("view": true, "cxx": "uint16", "cxx_name": "", "renamed_from": [], "file")j") !=
          std::string::npos);
}

TEST_CASE("the view words are checked: a flag is (@view) or (@view no), @cxx is a closed list") {
    const auto errors = [](std::string_view text) {
        shape::Set set;
        shape::Report report;
        add_text(set, text, report);
        return texts(report.errors);
    };
    const auto has = [](const std::vector<std::string>& e, std::string_view part) {
        return std::any_of(e.begin(), e.end(),
                           [&](const std::string& s) { return s.find(part) != std::string::npos; });
    };
    CHECK(has(errors("#shape (s (@view yes) (x int))"), "shape 's' is (@view) or (@view no)"));
    CHECK(has(errors("#shape (s (x int (@view maybe)))"), "field 'x' is (@view) or (@view no)"));
    CHECK(has(errors("#shape (s (x int (@cxx float)))"), "field 'x' (@cxx T): T is int8"));
    CHECK(has(errors("#shape (s (x float (@cxx int32)))"), "field 'x' (@cxx T)"));
    CHECK(has(errors("#shape (s (x symbol (@cxx int32)))"), "field 'x' (@cxx T)"));
    CHECK(has(errors("#shape (s (x int (@cxx_name 2fast)))"), "N is a C++ identifier"));
    CHECK(has(errors("#shape (s (@cxx_name \"S\") (x int))"), "shape 's' (@cxx_name N)"));
    CHECK(has(errors("#shape (s (@cxx int64) (x int))"), "has unknown metadata '@cxx'"));
    CHECK(errors("#shape (s (@view) (x int (@cxx int64) (@cxx_name _x) (@view no)))").empty());
}

TEST_CASE("a softer pack may not change a harder pack's view layout") {
    LoadSession session;
    session.set_pack_levels({"engine", "game"});
    auto hard =
        ok(read_string("#pack (core (level engine))\n"
                       "#shape (s (@at ss) (@keyed) (@view) (x int (@cxx int64)) (y int))",
                       "hard.dta", session));
    auto soft =
        ok(read_string("#pack (mod (level game) (requires core))\n"
                       "#shape (s (x int (@cxx int32)) (y int (@view no)))",
                       "soft.dta", session));
    shape::Set set;
    shape::Report report;
    set.add(session, report);
    CHECK(report.errors.empty());
    REQUIRE(report.warnings.size() == 2);
    CHECK(set.find("s")->field("x")->cxx == Symbol("int64"));
    CHECK(set.find("s")->field("y")->view);
}

// --- Views: the digest and the contract set. ---

namespace {

uint64_t digest_of(std::string_view text, std::string_view shape) {
    shape::Set set;
    shape::Report report;
    add_text(set, text, report);
    for (const Error& e : report.errors) MESSAGE(to_string(e));
    REQUIRE(report.errors.empty());
    return shape::view_digest(set, Symbol(shape));
}

std::string hex16(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace

TEST_CASE("a view's digest is symbol_hash of the shape's layout, and of nothing else") {
    const uint64_t d = digest_of(
        "#shape (move (@at fighters * moves) (@keyed) (@view) (@cxx_name MoveView)\n"
        "   (startup int (@range 1 60) (@kind frames) (@desc \"d\") (@cxx uint16))\n"
        "   (damage float (@cxx double) (@cxx_name dmg) (@step 0.5) (@note \"n\"))\n"
        "   (on_hit any (@view no))\n"
        "   (button symbol (@one_of light heavy) (@optional))\n"
        "   (@rule \"m\" {> $damage 0}))",
        "move");
    CHECK(d ==
          symbol_hash("move\nkeyed\nstartup\nint\nuint16\ndamage\nfloat\ndouble\nbutton\nsymbol"));
    // The same layout, without any tuning word, hashes the same.
    CHECK(
        d ==
        digest_of(
            "#shape (move (@at m) (@keyed) (startup int (@cxx uint16)) (damage float (@cxx double))"
            " (on_hit any (@view no)) (button symbol))",
            "move"));
    // What moves it: a type, a name, the order, a @cxx, a field left out, keyed.
    const auto moved = [&](std::string_view text) { return digest_of(text, "move") != d; };
    CHECK(moved(
        "#shape (move (@at m) (@keyed) (startup float (@cxx double)) (damage float (@cxx double))"
        " (on_hit any (@view no)) (button symbol))"));
    CHECK(
        moved("#shape (move (@at m) (@keyed) (startup int (@cxx uint16)) (dmg float (@cxx double))"
              " (on_hit any (@view no)) (button symbol))"));
    CHECK(moved(
        "#shape (move (@at m) (@keyed) (damage float (@cxx double)) (startup int (@cxx uint16))"
        " (on_hit any (@view no)) (button symbol))"));
    CHECK(moved(
        "#shape (move (@at m) (@keyed) (startup int (@cxx uint32)) (damage float (@cxx double))"
        " (on_hit any (@view no)) (button symbol))"));
    CHECK(moved(
        "#shape (move (@at m) (@keyed) (startup int (@cxx uint16)) (damage float (@cxx double))"
        " (on_hit any (@view no)) (button symbol (@view no)))"));
    CHECK(
        moved("#shape (move (startup int (@cxx uint16)) (damage float (@cxx double))"
              " (on_hit any (@view no)) (button symbol))"));
    CHECK(shape::view_digest(shape::Set(), Symbol("move")) == 0);
}

TEST_CASE("the digest spells lists, keys and nested shapes canonically") {
    shape::Set set;
    shape::Report report;
    add_text(set,
             "#shape (vec (x float) (y float))\n"
             "#shape (unit (@at units) (@keyed) (@view)\n"
             "   (pos (list float 3)) (tags (list symbol 1 4)) (next (list (key_of units)))\n"
             "   (weapon (key_of items.weapons)) (at vec) (path (list vec)))\n"
             "#shape (loop (self loop) (n int))",
             report);
    REQUIRE(report.errors.empty());
    const std::string vec = hex16(shape::view_digest(set, Symbol("vec")));
    CHECK(shape::view_digest(set, Symbol("vec")) == symbol_hash("vec\nx\nfloat\ny\nfloat"));
    CHECK(shape::view_digest(set, Symbol("unit")) ==
          symbol_hash("unit\nkeyed\npos\nlist(float,3)\ntags\nlist(symbol,1,4)\nnext\n"
                      "list(key_of(units))\nweapon\nkey_of(items.weapons)\nat\nshape(vec," +
                      vec + ")\npath\nlist(shape(vec," + vec + "))"));
    CHECK(shape::view_digest(set, Symbol("loop")) ==
          symbol_hash("loop\nself\nshape(loop,cycle)\nn\nint"));
}

TEST_CASE("the contract set holds the named layers' shapes, outside every #ifdef") {
    LoadSession session;
    session.define("PC");
    auto core = ok(read_string("#shape (a (x int))\n", "core.dta", session));
    auto content = ok(read_string("#shape (b (y int))\n", "content.dta", session));
    auto mod = ok(read_string("#shape (c (z int))\n#shape (a (w int))\n", "mod.dta", session));
    const Symbol roots[] = {Symbol("core.dta"), Symbol("content.dta")};
    shape::Report report;
    const shape::Set set = shape::contract_set(session, roots, report);
    CHECK(report.errors.empty());
    CHECK(set.find("a"));
    CHECK(set.find("b"));
    CHECK_FALSE(set.find("c"));
    CHECK_FALSE(set.find("a")->field("w"));  // the mod's addition stays out of the contract

    // A shape inside a branch the read took, one in the #else taken, and one skipped.
    auto taken = ok(read_string("#ifdef PC\n#shape (p (x int))\n#endif\n", "taken.dta", session));
    auto elsed = ok(read_string("#ifdef HANDHELD\n(a 1)\n#else\n#shape (q (x int))\n#endif\n",
                                "else.dta", session));
    auto skipped =
        ok(read_string("#ifdef HANDHELD\n#shape (r (x int))\n#endif\n#shape (s (x int))\n",
                       "skipped.dta", session));
    const Symbol all[] = {Symbol("taken.dta"), Symbol("else.dta"), Symbol("skipped.dta")};
    shape::Report r2;
    const shape::Set s2 = shape::contract_set(session, all, r2);
    const auto e = texts(r2.errors);
    REQUIRE(e.size() == 3);
    CHECK(e[0].find("shape 'p' is declared inside #ifdef PC") != std::string::npos);
    CHECK(e[0].find("taken.dta:2") != std::string::npos);
    CHECK(e[1].find("shape 'q' is declared inside #ifdef HANDHELD") != std::string::npos);
    CHECK(e[2].find("a #shape inside an #ifdef branch this build skipped") != std::string::npos);
    CHECK(e[2].find("skipped.dta:2") != std::string::npos);
    CHECK(s2.find("s"));
    CHECK_FALSE(s2.find("r"));
    // The skipped one is the session's to list, located.
    REQUIRE(session.skipped_shapes().size() == 1);
    CHECK(session.skipped_shapes()[0].root == Symbol("skipped.dta"));
    // A file outside the contract may hold them.
    shape::Report r3;
    const Symbol only[] = {Symbol("core.dta")};
    (void)shape::contract_set(session, only, r3);
    CHECK(r3.errors.empty());
}

// --- Generating views: which shapes, the names, and the golden headers. ---

namespace {

// The shapes the golden headers and the data tests' reader are generated from.
const char* const kViewShapes =
    "#shape (vec2 (@view) (x float) (y float))\n"
    "#shape (move (@at fighters * moves) (@keyed) (@view)\n"
    "   (startup     int (@range 1 60) (@cxx uint16))\n"
    "   (damage      float (@renamed_from hurt))\n"
    "   (name        string (@optional))\n"
    "   (button      symbol (@optional))\n"
    "   (armored     bool (@optional))\n"
    "   (cancel_into (list symbol) (@optional))\n"
    "   (hitbox      vec2 (@optional))\n"
    "   (points      (list vec2 1 4) (@optional))\n"
    "   (ramp        curve (@optional))\n"
    "   (on_hit      any (@view no)))\n"
    "#shape (scalars (@view) (startup int (@cxx uint16)) (damage float) (name string)\n"
    "   (button symbol) (armored bool))\n"
    "#shape (item (@at items) (@keyed) (@view) (price float) (tag string (@optional))\n"
    "   (sizes (list int) (@optional)))\n";

std::string generated(const shape::Set& set, const std::string& file) {
    std::string out;
    const shape::ViewOptions o{.ns = "views_test"};
    const auto base = file.substr(0, file.find('.'));
    if (file == "views.hpp") {
        const Symbol views[] = {Symbol("item"), Symbol("move"), Symbol("scalars"), Symbol("vec2")};
        shape::write_views_umbrella(set, views, o, string_out(out));
    } else if (base.ends_with("_read")) {
        shape::write_view_reader(set, Symbol(base.substr(0, base.size() - 5)), o, string_out(out));
    } else {
        shape::write_view_header(set, Symbol(base), o, string_out(out));
    }
    return out;
}

}  // namespace

TEST_CASE("view_shapes: the marked and listed shapes and what they nest, sorted; its errors") {
    shape::Set set;
    shape::Report report;
    add_text(set,
             "#shape (b (@view) (inner a))\n"
             "#shape (a (x int))\n"
             "#shape (c (y int))\n"
             "#shape (d (z int))\n",
             report);
    REQUIRE(report.errors.empty());
    const Symbol listed[] = {Symbol("c")};
    const auto views = shape::view_shapes(set, listed, report);
    CHECK(report.errors.empty());
    REQUIRE(views.size() == 3);
    CHECK(views[0] == Symbol("a"));  // nested by b, so generated too
    CHECK(views[1] == Symbol("b"));
    CHECK(views[2] == Symbol("c"));

    const auto errors = [](std::string_view text, std::initializer_list<const char*> listed_names) {
        shape::Set s;
        shape::Report r;
        add_text(s, text, r);
        REQUIRE(r.errors.empty());
        std::vector<Symbol> l;
        for (const char* n : listed_names) l.emplace_back(n);
        (void)shape::view_shapes(s, l, r);
        return texts(r.errors);
    };
    const auto has = [](const std::vector<std::string>& e, std::string_view part) {
        return std::any_of(e.begin(), e.end(),
                           [&](const std::string& s) { return s.find(part) != std::string::npos; });
    };
    CHECK(has(errors("#shape (a (x int))", {"nope"}),
              "the project lists a view of shape 'nope', which no contract layer declares"));
    CHECK(has(errors("#shape (a (@view) (x any))", {}),
              "field 'x' of shape 'a' is `any`, which no struct can hold: mark it (@view no)"));
    CHECK(has(errors("#shape (a (@view) (x (list any)))", {}), "is `any`"));
    CHECK(has(errors("#shape (a (@view) (b b))\n#shape (b (a a))", {}),
              "shape 'a' nests itself, so its view cannot be a struct"));
    CHECK(has(errors("#shape (a (@view) (b (list b)))\n#shape (b (a a))", {}), "nests itself"));
    CHECK(has(errors("#shape (a (@view) (b c))", {}), "shape 'a' nests the unknown shape 'c'"));
    CHECK(has(errors("#shape (a (@view) (default int))", {}),
              "field 'default' of shape 'a' would generate the member `default`"));
    // `class` cases to `Class`, which is fine; a member keeps its name, so `default` is not.
    CHECK(errors("#shape (class (@view) (x int))", {}).empty());
    CHECK(has(errors("#shape (a (@view) (x int) (y int (@cxx_name x)))", {}),
              "generates the member `x`, which another field does too"));
    CHECK(
        has(errors("#shape (boss_phase (@view) (x int))\n#shape (boss__phase (@view) (y int))", {}),
            "shapes 'boss__phase' and 'boss_phase' both generate `BossPhase`"));
    CHECK(has(errors("#shape (a (@view) (x int (@cxx_name int)))", {}), "member `int`"));
}

TEST_CASE("the generated headers match their golden files (BAKER_UPDATE_GOLDEN=1 rewrites them)") {
    shape::Set set;
    shape::Report report;
    add_text(set, kViewShapes, "fighters.dta", report);
    REQUIRE(report.errors.empty());
    const auto views = shape::view_shapes(set, {}, report);
    REQUIRE(report.errors.empty());
    REQUIRE(views.size() == 4);
    const bool update = std::getenv("BAKER_UPDATE_GOLDEN") != nullptr;
    for (const char* file :
         {"vec2.hpp", "vec2_read.hpp", "move.hpp", "move_read.hpp", "scalars.hpp",
          "scalars_read.hpp", "item.hpp", "item_read.hpp", "views.hpp"}) {
        const std::string path = kDir + "/views/" + file;
        const std::string text = generated(set, file);
        if (update) {
            std::ofstream(path) << text;
            continue;
        }
        std::ifstream in(path);
        REQUIRE_MESSAGE(in, path);
        const std::string golden((std::istreambuf_iterator<char>(in)), {});
        CHECK_MESSAGE(text == golden, file);
    }
    // The digest in the struct is the shape's.
    char hex[32];
    std::snprintf(hex, sizeof hex, "0x%016llxull",
                  static_cast<unsigned long long>(shape::view_digest(set, Symbol("move"))));
    CHECK(generated(set, "move.hpp").find(hex) != std::string::npos);
}

// --- Drift: the views a game compiled against its shapes. ---

TEST_CASE("check_views: a view generated from another layout, a missing shape, a nameless digest") {
    shape::Set set;
    shape::Report report;
    add_text(set, "#shape (a (@view) (x int))\n#shape (b (@view) (y float))", report);
    REQUIRE(report.errors.empty());
    const uint64_t a = shape::view_digest(set, Symbol("a"));
    const uint64_t b = shape::view_digest(set, Symbol("b"));
    const ViewInfo same[] = {{"a", a}, {"b", b}, {"", a}};
    shape::check_views(set, same, report);
    CHECK(report.warnings.empty());
    const ViewInfo drifted[] = {{"a", a ^ 1}, {"c", 7}, {"", 9}};
    shape::check_views(set, drifted, report);
    const auto w = texts(report.warnings);
    REQUIRE(w.size() == 3);
    CHECK(w[0].find("view 'a' was generated from another layout of its shape") !=
          std::string::npos);
    CHECK(w[0].find("t.dta:1") != std::string::npos);
    CHECK(w[1].find("view 'c' names a shape no contract layer declares") != std::string::npos);
    CHECK(w[2].find("a compiled view (digest 0x0000000000000009) matches no shape's layout") !=
          std::string::npos);
}

TEST_CASE("contract_roots: a file layer's own file, and the files under a directory layer") {
    std::map<std::string, std::string> files = {
        {"core.dta", "#shape (a (@view) (x int))\n(a (x 1))"},
        {"mods/one.dta", "#shape (a (y int))"},
        {"mods/two.dta", "#include \"inc.dta\"\n"},
        {"mods/inc.dta", "(b 2)"}};
    Config cfg({.tweaks = nullptr});
    cfg.session().set_reader([&](std::string_view p, TextSink out) {
        const auto it = files.find(std::string(p));
        if (it == files.end()) return false;
        out(it->second);
        return true;
    });
    cfg.session().set_lister([&](std::string_view dir, Callback<void(std::string_view)> entry) {
        if (dir != "mods") return false;
        entry("mods/one.dta");
        entry("mods/two.dta");
        return true;
    });
    REQUIRE(cfg.push_layer("core", "core.dta"));
    REQUIRE(cfg.push_layer_dir("mods", "mods"));
    const Symbol core_only[] = {Symbol("core")};
    const auto r1 = shape::contract_roots(cfg, core_only);
    REQUIRE(r1.size() == 1);
    CHECK(r1[0] == Symbol("core.dta"));
    const Symbol both[] = {Symbol("core"), Symbol("mods")};
    const auto r2 = shape::contract_roots(cfg, both);
    std::vector<std::string> names;
    for (const Symbol s : r2) names.emplace_back(s.str());
    CHECK(std::find(names.begin(), names.end(), "core.dta") != names.end());
    CHECK(std::find(names.begin(), names.end(), "mods/one.dta") != names.end());
    CHECK(std::find(names.begin(), names.end(), "mods/two.dta") != names.end());
    // The contract set over them: the mod's field is in, the include contributes nothing.
    shape::Report report;
    const shape::Set set = shape::contract_set(cfg.read_session(), r2.items(), report);
    CHECK(report.errors.empty());
    CHECK(set.find("a")->field("y"));
}

// --- Aliases: a key_of resolves through (@aka old), and says so. ---

TEST_CASE("a key_of names a record by its alias, and the report lists the keys so resolved") {
    const auto r = check_text(R"(#shape (unit (@at units) (@keyed) (weapon (key_of weapons)))
        (weapons (plasma (@aka laser beam) (damage 3)) (cannon (damage 9)))
        (units (scout (weapon laser)) (tank (weapon cannon)) (odd (weapon lazer))))");
    const auto e = texts(r.errors);
    REQUIRE(e.size() == 1);
    CHECK(e[0].find("'lazer' is not a key of weapons (laser?)") != std::string::npos);
    REQUIRE(r.aliases_used.size() == 1);
    CHECK(r.aliases_used[0] == Symbol("laser"));
}
