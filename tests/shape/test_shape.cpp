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

// --- As JSON, baker.shapes/1.1. ---

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
    CHECK(json.rfind("{\n  \"schema\": \"baker.shapes/1.1\",\n  \"shapes\": [\n", 0) == 0);
    CHECK(json.find(R"j({"name": "boss_phase", "at": ["bosses", "*", "phases"], "keyed": true, )j"
                    R"j("file": "shapes.dta", "line": 2, "fields": [)j") != std::string::npos);
    CHECK(
        json.find(
            R"j({"name": "enter_at", "type": "float", "element": "", "shape": "", )j"
            R"j("optional": false, "range": [0.0, 1.0], "step": 0.05, "kind": "", )j"
            R"j("desc": "HP fraction", "note": "", "one_of": [], "file": "shapes.dta", "line": 3})j") !=
        std::string::npos);
    CHECK(json.find(R"j({"name": "adds", "type": "list", "element": "symbol", )j"
                    R"j("shape": "", "optional": true, "range": null,)j") != std::string::npos);
}
