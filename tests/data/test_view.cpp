// Views, the runtime side: read_view over a record and over
// a Config, each field type, the located errors, defaults, borrowing across a reload, and
// that a read allocates nothing. The readers are tests/shape/views/, the golden headers
// the generator's test writes, so a change to the generator is tested here too.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/view_read.hpp>

#include "alloc_counter.hpp"
#include "move_read.hpp"
#include "ok.hpp"
#include "scalars_read.hpp"
#include "views.hpp"

#include <map>
#include <string>
#include <vector>

using namespace baker;
using baker::test::ok;
using views_test::Move;
using views_test::Vec2;

namespace {

const char* const kFighters =
    "(fighters\n"
    "  (kestrel (moves\n"
    "    (jab (startup 4) (damage 3.5) (name \"Jab\") (button light) (armored 1)\n"
    "         (cancel_into heavy kick) (hitbox (x 1) (y 2.5))\n"
    "         (points (p (x 0) (y 0)) (p (x 3) (y 1)))\n"
    "         (ramp (0 0) (1 1)) (on_hit {do_hit}) (taunt \"ha\"))\n"
    "    (heavy (startup 9) (damage 12) (@note \"tuned\") (points (p (x 1) (y 1)) (@desc \"d\")))\n"
    "    (old (startup 2) (hurt 7))\n"
    "    (bad_type (startup 4.5) (damage 1))\n"
    "    (bad_range (startup 70000) (damage 1))\n"
    "    (bad_float (startup 1) (damage 1e300))\n"
    "    (bad_count (startup 1) (damage 1)\n"
    "         (points (p (x 1) (y 1)) (p (x 1) (y 1)) (p (x 1) (y 1)) (p (x 1) (y 1)) (p (x 1) (y "
    "1))))\n"
    "    (no_damage (startup 1))\n"
    "    (bad_nested (startup 1) (damage 1) (hitbox (x 1)))\n"
    "    (bad_list (startup 1) (damage 1) (cancel_into heavy 7))\n"
    "    (many (startup 1 2) (damage 1)))))\n";

std::string text(const Error& e) {
    return std::string(to_string(e));
}

}  // namespace

TEST_CASE("read_view reads every field type from a record, and defaults the absent ones") {
    LoadSession session;
    auto root = ok(read_string(kFighters, "f.dta", session));
    const Array* jab = root->try_get("fighters.kestrel.moves.jab");
    REQUIRE(jab);
    const Move m = ok(read_view<Move>(*jab));
    CHECK(m.startup == 4);
    CHECK(m.damage == 3.5f);
    CHECK(m.name == "Jab");
    CHECK(m.button == Symbol("light"));
    CHECK(m.armored);
    REQUIRE(m.cancel_into.size() == 2);
    CHECK(m.cancel_into[0] == Symbol("heavy"));
    CHECK(ok(m.cancel_into.at(1)) == Symbol("kick"));
    CHECK(m.hitbox.x == 1.0f);
    CHECK(m.hitbox.y == 2.5f);
    REQUIRE(m.points.size() == 2);
    CHECK(m.points[1].x == 3.0f);
    CHECK(ok(m.points.at(1)).y == 1.0f);
    CHECK(m.ramp.sample(0.5) == doctest::Approx(0.5));
    // Iteration, as a range.
    std::vector<Symbol> cancels;
    for (const Symbol s : m.cancel_into) cancels.push_back(s);
    CHECK(cancels.size() == 2);
    // Absent optional fields are the defaults; metadata and undeclared fields are skipped.
    const Move h = ok(read_view<Move>(*root->try_get("fighters.kestrel.moves.heavy")));
    CHECK(h.startup == 9);
    CHECK(h.name.empty());
    CHECK(h.button.empty());
    CHECK_FALSE(h.armored);
    CHECK(h.cancel_into.empty());
    CHECK(h.hitbox.x == 0);
    CHECK(h.points.size() == 1);
    CHECK(h.ramp.empty());
}

TEST_CASE("a field spelled by a name its shape renamed reads as the field") {
    LoadSession session;
    auto root = ok(read_string(kFighters, "f.dta", session));
    const Array* old = root->try_get("fighters.kestrel.moves.old");
    REQUIRE(old);
    const Move m = ok(read_view<Move>(*old));  // (hurt 7) is damage, and damage is not missing
    CHECK(m.damage == 7.0f);
}

TEST_CASE("a packed list, a baked table's, reads its elements directly") {
    const Symbol names[] = {Symbol("heavy"), Symbol("kick")};
    const auto list = ListView<Symbol>::packed(names, 2);
    CHECK(list.is_packed());
    CHECK(list.size() == 2);
    CHECK(list[1] == Symbol("kick"));
    CHECK(list[2] == Symbol());
    CHECK(ok(list.at(0)) == Symbol("heavy"));
    CHECK_FALSE(list.at(2));
    CHECK(text(list.at(2).error()).find("past the packed list's 2") != std::string::npos);
    int n = 0;
    for (const Symbol s : list) n += s.empty() ? 0 : 1;
    CHECK(n == 2);
    CHECK(list.elements().size() == 2);
    const ListView<float> from_tree;
    CHECK(from_tree.elements().empty());
    // The generated fix-up lists name what a table relocates, with the nested views' own.
    const auto fixups = views_test::Move::fixups();
    REQUIRE(fixups.size() == 6);
    CHECK(fixups[0].kind == ViewFixupKind::String);
    CHECK(fixups[0].offset == offsetof(views_test::Move, name));
    CHECK(fixups[2].kind == ViewFixupKind::List);
    CHECK(fixups[2].element == ViewFixup::Element::Symbol);
    CHECK(fixups[2].read == nullptr);
    CHECK(fixups[2].element_size == sizeof(Symbol));
    CHECK(fixups[3].kind == ViewFixupKind::Struct);
    CHECK(fixups[3].nested_count == 0);  // vec2 has two floats
    CHECK(fixups[3].read != nullptr);
    CHECK(fixups[4].element == ViewFixup::Element::Struct);
    CHECK(fixups[4].element_size == sizeof(views_test::Vec2));
    CHECK(fixups[5].kind == ViewFixupKind::Curve);
    CHECK(views_test::Move::kAt == "fighters * moves");
    CHECK(views_test::Move::kKeyed);
    CHECK(views_test::Vec2::fixups().empty());
    CHECK(views_test::Vec2::kAt.empty());
}

TEST_CASE("a view's errors name the field, located where it is written") {
    LoadSession session;
    auto root = ok(read_string(kFighters, "f.dta", session));
    const auto fails = [&](const char* key) {
        auto r =
            read_view<Move>(*root->try_get(Path(std::string("fighters.kestrel.moves.") + key)));
        REQUIRE_FALSE(r);
        return text(r.error());
    };
    CHECK(fails("bad_type").find("field 'startup' of 'move' is not an int: 4.5") !=
          std::string::npos);
    CHECK(fails("bad_type").find("f.dta:9") != std::string::npos);
    CHECK(fails("bad_range").find("field 'startup' of 'move' is 70000, outside uint16") !=
          std::string::npos);
    CHECK(fails("bad_float").find("field 'damage' of 'move' is 1e") != std::string::npos);
    CHECK(fails("bad_float").find(", outside float") != std::string::npos);
    CHECK(fails("bad_count").find("field 'points' of 'move' holds 5 values, not 1 to 4") !=
          std::string::npos);
    CHECK(fails("no_damage").find("'move' has no 'damage'") != std::string::npos);
    CHECK(fails("no_damage").find("f.dta:14") != std::string::npos);
    CHECK(fails("bad_nested").find("'vec2' has no 'y'") != std::string::npos);
    CHECK(fails("many").find("field 'startup' of 'move' is not an int: (startup 1 2)") !=
          std::string::npos);
    // A list converts as it is read: a bad element is an error at that read, not the view's.
    const Move bad_list = ok(read_view<Move>(*root->try_get("fighters.kestrel.moves.bad_list")));
    CHECK(bad_list.cancel_into[0] == Symbol("heavy"));
    CHECK(bad_list.cancel_into[1].empty());
    CHECK(bad_list.cancel_into[5].empty());
    auto e = bad_list.cancel_into.at(1);
    REQUIRE_FALSE(e);
    CHECK(text(e.error()).find("element 1 of 'cancel_into' is not a symbol: 7") !=
          std::string::npos);
}

TEST_CASE("a view reads through a Config, by path and through get<T>, and names the path") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("f", kFighters));
    const Move m = ok(read_view<Move>(cfg, "fighters.kestrel.moves.jab"));
    CHECK(m.startup == 4);
    // The generated Convert<T> lets get<T> read a view the usual way, and nest one.
    const Move g = ok(cfg.get<Move>("fighters.kestrel.moves.jab"));
    CHECK(g.damage == 3.5f);
    const Vec2 hb = ok(cfg.get<Vec2>("fighters.kestrel.moves.jab.hitbox"));
    CHECK(hb.y == 2.5f);
    auto missing = read_view<Move>(cfg, "fighters.kestrel.moves.nothing");
    REQUIRE_FALSE(missing);
    CHECK(text(missing.error()).find("fighters/kestrel/moves/nothing") != std::string::npos);
    auto bad = read_view<Move>(cfg, "fighters.kestrel.moves.bad_type");
    REQUIRE_FALSE(bad);
    CHECK(text(bad.error()).find("fighters/kestrel/moves/bad_type") != std::string::npos);
    CHECK(text(bad.error()).find("not an int") != std::string::npos);
}

TEST_CASE(
    "a view's strings and lists borrow from the tree, which a held Ref keeps across a reload") {
    if constexpr (!kShipping) {
        std::map<std::string, std::string> files = {{"f.dta", kFighters}};
        Config cfg({.tweaks = nullptr});
        cfg.session().set_reader([&](std::string_view p, TextSink out) {
            const auto it = files.find(std::string(p));
            if (it == files.end()) return false;
            out(it->second);
            return true;
        });
        REQUIRE(cfg.push_layer("f", "f.dta"));
        const Ref<Array> held = cfg.root();
        const Move before = ok(read_view<Move>(cfg, "fighters.kestrel.moves.jab"));
        files["f.dta"] =
            "(fighters (kestrel (moves (jab (startup 5) (damage 1) (name \"Hook\")))))";
        REQUIRE(cfg.reload());
        const Move after = ok(read_view<Move>(cfg, "fighters.kestrel.moves.jab"));
        CHECK(after.name == "Hook");
        CHECK(after.startup == 5);
        CHECK(before.name == "Jab");  // the held tree is alive, and unchanged
        CHECK(before.cancel_into[1] == Symbol("kick"));
    }
}

TEST_CASE("reading a view allocates nothing") {
    LoadSession session;
    auto root = ok(read_string(kFighters, "f.dta", session));
    const Array* jab = root->try_get("fighters.kestrel.moves.jab");
    Move m;
    Error err;
    (void)ViewReader<Move>::read(*jab, m, &err);  // warm: the literals intern once
    if (test::counting_allocations()) {
        test::AllocationScope scope;
        for (int i = 0; i < 100; ++i) {
            CHECK(ViewReader<Move>::read(*jab, m, &err));
            (void)m.cancel_into[1];
            (void)m.points[0];
        }
        CHECK(scope.count() == 0);
    }
}
