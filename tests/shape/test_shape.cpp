// baker::shape: declarations, checking, enumeration, and the Config hook.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/shape.hpp>
#include <baker/std_interop.hpp>

#include "alloc_counter.hpp"
#include "ok.hpp"

#include <algorithm>
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

shape::Report check_text(std::string_view text) {
    auto tree = ok(read_string(text, "t.dta"));
    shape::Set set;
    shape::Report report;
    set.add(*tree, report);
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
              "/bosses.dta:23, at "
              "bosses/boss/phases/boss_phase/enter_at)"));
    CHECK(has("'boss_phase' has no 'cadence'  (" + kDir + "/bosses.dta:30"));
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
        (shapes
          (vec (x float) (y float))
          (unit (name string) (alive bool) (speed float (range 0 10)) (tags (list symbol))
                (pos vec) (path (list vec)) (data any) (count int)))
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
        (shapes (attack (dmg int (range 0 10))) (boss (attacks (list attack)) (main attack)))
        (boss (attacks (attack (dmg 50)) (attack (dmg 1))) (main (dmg 11)))
        (attack (dmg 12))
    )");
    CHECK(texts(r.errors).size() == 3);  // 50, 11, and the loose attack's 12
}

TEST_CASE("checking allocates the same for 10 instances as for 1,000") {
    auto allocations_for = [](int units) {
        std::string text = "(shapes (vec (x float) (y float)) (unit (hp int) (pos vec)))";
        for (int i = 0; i < units; ++i) text += "(unit (hp 1) (pos (x 1) (y 2)))";
        auto tree = ok(read_string(text, "t.dta"));
        shape::Set set;
        shape::Report report;
        set.add(*tree, report);
        const test::AllocationScope scope;
        shape::check(*tree, set, report);
        return scope.count();
    };
    if (!test::counting_allocations()) return;
    CHECK(allocations_for(1000) == allocations_for(10));
}

TEST_CASE("malformed declarations are located errors; unknown shapes are reported") {
    const auto r = check_text(R"((shapes (s (a) (b float (range 2 1)) (c (list)) 5) 7)
                                  (s (c 1) (d 2)) (shapes (t (u nowhere))) (t (u 1)))");
    const auto e = texts(r.errors);
    CHECK(e.size() == 6);
    CHECK(std::count_if(e.begin(), e.end(), [](const std::string& s) {
              return s.find("t.dta:1)") != std::string::npos;
          }) == 5);
    CHECK(std::any_of(e.begin(), e.end(), [](const std::string& s) {
        return s.find("'u' is of the unknown shape 'nowhere'") != std::string::npos;
    }));
}

TEST_CASE("shapes are enumerable, with their metadata, for tuning panels") {
    auto tree = ok(read_file(kDir + "/bosses.dta"));
    auto patch = ok(read_file(kDir + "/bosses_patch.dta"));
    shape::Set set;
    shape::Report report;
    set.add(*tree, report);
    set.add(*patch, report);
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
        (shapes
          (move (at fighters * moves) (keyed)
             (startup int (range 1 60))
             (cancels (list symbol) (optional))))
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

    const auto bad = check_text("(shapes (move (keyed) (startup int)))");
    REQUIRE(bad.errors.size() == 1);
    CHECK(texts(bad.errors)[0].find("no (at ...)") != std::string::npos);
}

TEST_CASE("a softer pack may narrow a harder pack's shape, not loosen it") {
    static const std::map<std::string, std::string> files = {
        {"base.dta",
         "(pack game_rules (level game))\n"
         "(shapes (player (speed int (range 1 10)) (name symbol)))\n(player (speed 5) (name p))"},
        {"mod.dta",
         "(pack fast_mod (level mod))\n"
         "(shapes (player (speed int (range 1 1000)) (name symbol (optional))))\n"
         "(player (speed 900))"},
        {"tight.dta",
         "(pack tight_mod (level mod))\n(shapes (player (speed int (range 2 8))))\n"
         "(player (speed 9))"},
        {"retype.dta", "(pack retype_mod (level mod))\n(shapes (player (speed float)))"},
        {"loose_base.dta", "(shapes (player (speed int (range 1 10))))\n(player (speed 5))"},
        {"loose_mod.dta", "(shapes (player (speed int (range 1 1000))))\n(player (speed 900))"},
        {"buttons.dta",
         "(pack game_rules (level game))\n"
         "(shapes (move (button symbol (one_of light heavy))))\n(move (button light))"},
        {"more_buttons.dta",
         "(pack more_mod (level mod))\n(shapes (move (button symbol (one_of kick))))\n"
         "(move (button kick))"}};
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
    // Files that declare no pack are unconstrained, as before.
    Run loose{"loose_base.dta", "loose_mod.dta"};
    CHECK(loose.reported.empty());
}

TEST_CASE("a symbol field lists the symbols it allows, (one_of ...)") {
    const char* const kShapes =
        "(shapes (move (button symbol (one_of light heavy kick special))"
        " (combo (list symbol) (one_of light heavy)) (startup int (optional))))\n";
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
    set.add(*ok(read_string("(shapes (move (button symbol (one_of light heavy))))", "a.dta")),
            report);
    set.add(*ok(read_string("(shapes (move (button symbol (one_of special light))))", "b.dta")),
            report);
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
    CHECK(texts(check_text("(shapes (s (n int (one_of a b))))").errors) ==
          std::vector<std::string>{
              "baker: field 'n' lists (one_of ...) symbols, so its type is symbol or (list "
              "symbol)  (t.dta:1)"});
    CHECK_FALSE(check_text("(shapes (s (n symbol (one_of a 2))))").ok());
}

// --- As JSON, baker.shapes/1.4. ---

TEST_CASE("the shapes as JSON: each shape and its fields") {
    shape::Set shapes;
    shape::Report report;
    shapes.add(
        *ok(read_string("(shapes\n"
                        "   (boss_phase (at bosses * phases) (keyed)\n"
                        "      (enter_at float (range 0 1) (step 0.05) (desc \"HP fraction\"))\n"
                        "      (adds (list symbol) (optional))))\n",
                        "shapes.dta")),
        report);
    REQUIRE(report.errors.empty());
    std::string json;
    shape::write_json(shapes, string_out(json));
    CHECK(json.rfind("{\n  \"schema\": \"baker.shapes/1.4\",\n  \"shapes\": [\n", 0) == 0);
    CHECK(json.find(R"j({"name": "boss_phase", "at": ["bosses", "*", "phases"], "keyed": true, )j"
                    R"j("file": "shapes.dta", "line": 2, "fields": [)j") != std::string::npos);
    CHECK(
        json.find(
            R"j({"name": "enter_at", "type": "float", "element": "", "shape": "", "table": "", )j"
            R"j("optional": false, "range": [0.0, 1.0], "step": 0.05, "kind": "", )j"
            R"j("desc": "HP fraction", "note": "", "one_of": [], "file": "shapes.dta", "line": 3})j") !=
        std::string::npos);
    CHECK(json.find(R"j({"name": "adds", "type": "list", "element": "symbol", )j"
                    R"j("shape": "", "table": "", "optional": true, "range": null,)j") !=
          std::string::npos);
}

// --- Keys of a table, (key_of TABLE). ---

TEST_CASE("a key field names a record of its table; a typo is located, with the key meant") {
    static constexpr std::string_view kShapes =
        "(shapes (unit (at units) (keyed) (weapon (key_of weapons))\n"
        "   (spares (list (key_of weapons)) (optional)) (backup (key_of weapons) (optional))))\n"
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
    CHECK(check_text("(shapes (s (at uses) (keyed) (w (key_of items.weapons))))\n"
                     "(items (weapons (laser)))\n(uses (a (w laser)))")
              .ok());
    CHECK(texts(check_text("(shapes (s (at uses) (keyed) (w (key_of gear))))\n(uses (a (w x)))")
                    .errors) ==
          std::vector<std::string>{"baker: 'w' names a key of gear, and no table gear encloses it  "
                                   "(t.dta:2, at uses/a/w)"});
    // A path resolves by its first segment: the nearest `items` is the shop's, which has no
    // weapons, so the root's items.weapons is not reached.
    CHECK(texts(check_text("(shapes (s (at shop uses) (keyed) (w (key_of items.weapons))))\n"
                           "(items (weapons (laser)))\n(shop (items (food)) (uses (a (w laser))))")
                    .errors) ==
          std::vector<std::string>{"baker: 'w' names a key of items.weapons, and no table "
                                   "items.weapons encloses it  (t.dta:3, at shop/uses/a/w)"});
    CHECK_FALSE(check_text("(shapes (s (w (key_of))))").ok());
    CHECK_FALSE(check_text("(shapes (s (w (key_of 3))))").ok());
}

TEST_CASE("a key resolves in the nearest enclosing table: each fighter's own moves") {
    static constexpr std::string_view kShapes =
        "(shapes (move (at fighters * moves) (keyed) (cancel_into (list (key_of moves)) "
        "(optional))))\n";
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
         "(pack game_rules (level game))\n"
         "(shapes (unit (at units) (keyed) (weapon (key_of weapons))))\n"
         "(weapons (laser) (cannon))\n(units (scout (weapon laser)))"},
        {"add.dta", "(weapons (rail))\n(units (tank (weapon rail)))"},
        {"remove.dta", "(weapons (laser #remove))"},
        {"mod.dta", "(pack mod_weapons (level mod))\n(weapons (plasma))"},
        {"uses_mod.dta", "(pack game_rules (level game))\n(units (scout (weapon plasma)))"}};
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
    shapes.add(*ok(read_string("(shapes (unit (weapon (key_of items.weapons))\n"
                               "   (spares (list (key_of weapons)))))",
                               "shapes.dta")),
               report);
    REQUIRE(report.errors.empty());
    std::string json;
    shape::write_json(shapes, string_out(json));
    CHECK(json.find(R"j({"name": "weapon", "type": "key", "element": "", "shape": "", )j"
                    R"j("table": "items.weapons", )j") != std::string::npos);
    CHECK(json.find(R"j({"name": "spares", "type": "list", "element": "key", "shape": "", )j"
                    R"j("table": "weapons", )j") != std::string::npos);
}

// --- Rules, read here and evaluated by baker::tools. ---

TEST_CASE("a shape's rules are read, replaced by message, and malformed ones located") {
    shape::Set shapes;
    shape::Report report;
    shapes.add(
        *ok(read_string("(shapes (unit (at units) (keyed) (max_hp int) (start_hp int)\n"
                        "   (rule \"starts at or below its maximum\" {<= $start_hp $max_hp})\n"
                        "   (rule \"has some health\" {> $max_hp 0} (warn))))\n",
                        "base.dta")),
        report);
    REQUIRE(report.errors.empty());
    const shape::Shape* unit = shapes.find(Symbol("unit"));
    REQUIRE(unit);
    REQUIRE(unit->rules.size() == 2);
    CHECK(unit->rules[0].message == "starts at or below its maximum");
    CHECK_FALSE(unit->rules[0].warn);
    CHECK(unit->rules[1].warn);
    CHECK(unit->rules[0].line == 2);
    // A later layer softens the first and adds a third.
    shapes.add(*ok(read_string("(shapes (unit\n"
                               "   (rule \"starts at or below its maximum\" {<= $start_hp "
                               "$max_hp} (warn))\n"
                               "   (rule \"is named\" {!= $name 0})))\n",
                               "mod.dta")),
               report);
    unit = shapes.find(Symbol("unit"));
    REQUIRE(unit->rules.size() == 3);
    CHECK(unit->rules[0].warn);
    CHECK(unit->rules[0].file == Symbol("mod.dta"));

    CHECK(texts(check_text("(shapes (s (rule) (rule \"m\") (rule \"m\" 3) (rule \"m\" {f} x)))")
                    .errors) ==
          std::vector<std::string>{
              "baker: a rule is declared as (rule \"message\" {expression}), optionally (warn)  "
              "(t.dta:1)",
              "baker: a rule is declared as (rule \"message\" {expression}), optionally (warn)  "
              "(t.dta:1)",
              "baker: rule \"m\" holds 3, not an {expression}  (t.dta:1)",
              "baker: rule \"m\" has x after its expression; only (warn) may follow  (t.dta:1)"});
}

TEST_CASE("a softer pack may make a harder pack's rule an error, not soften or change it") {
    static const std::map<std::string, std::string> files = {
        {"base.dta",
         "(pack game_rules (level game))\n"
         "(shapes (unit (rule \"positive\" {> $hp 0})))\n"},
        {"soften.dta",
         "(pack mod (level mod))\n(shapes (unit (rule \"positive\" {> $hp 0} (warn))))\n"},
        {"change.dta", "(pack mod (level mod))\n(shapes (unit (rule \"positive\" {> $hp -5})))\n"},
        {"warned.dta",
         "(pack game_rules (level game))\n"
         "(shapes (unit (rule \"positive\" {> $hp 0} (warn))))\n"},
        {"harden.dta", "(pack mod (level mod))\n(shapes (unit (rule \"positive\" {> $hp 0})))\n"}};
    LoadSession session;
    session.set_reader([](std::string_view p, TextSink out) {
        const auto it = files.find(std::string(p));
        if (it == files.end()) return false;
        out(it->second);
        return true;
    });
    session.set_pack_levels({"game", "mod"});
    auto layered = [&](const char* below, const char* above) {
        shape::Set shapes;
        shape::Report report;
        shapes.add(*ok(read_file(below, session)), report, &session);
        shapes.add(*ok(read_file(above, session)), report, &session);
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
    const auto tree =
        ok(read_string("(shapes (unit (at units) (keyed) (rule \"r\" {f}))\n"
                       "        (boss_phase (rule \"p\" {g}))\n"
                       "        (plain (n int)))\n"
                       "(units (grunt (hp 1)) (tank (hp 2)))\n"
                       "(bosses (dragon (phases (boss_phase (at 1)) (boss_phase (at 2)))))\n"
                       "(plain (n 3))\n",
                       "t.dta"));
    shape::Set shapes;
    shape::Report report;
    shapes.add(*tree, report);
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
    shapes.add(*ok(read_string("(shapes (unit (hp int)\n"
                               "   (rule \"positive\" {> $hp 0} (warn))))\n",
                               "shapes.dta")),
               report);
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
    REQUIRE(cfg.push_layer("base", ok(read_string("(shapes (unit (rule \"r\" {f})))\n", "t.dta"))));
    REQUIRE(cfg.push_layer("more", ok(read_string("(x 1)\n", "u.dta"))));
    const auto told = std::count_if(reported.begin(), reported.end(), [](const std::string& r) {
        return r.find("has no baker::script to check them") != std::string::npos;
    });
    CHECK(told == (BAKER_TEST_SCRIPT ? 0 : 1));
}

TEST_CASE("a curve field holds a curve, and its range each point's y") {
    const char* const shapes =
        "(shapes (enemy (at enemies) (keyed)\n"
        "   (falloff curve (range 0 1))\n"
        "   (pace curve)))\n";
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
