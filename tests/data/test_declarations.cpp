// #pack, #origin and #shape: a file's declarations as directives, never in its tree.
#include <doctest.h>
#include <baker/cache.hpp>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>
#include <baker/write.hpp>

#include <string>
#include <vector>
#include "ok.hpp"
#include "temp_dir.hpp"

using namespace baker;
using baker::test::ok;
using baker::test::TempDir;

namespace {
LoadSession leveled() {
    LoadSession s;
    s.set_pack_levels({"game", "dlc", "mod"});
    return s;
}
std::string error_of(std::string_view text) {
    LoadSession s = leveled();
    auto r = read_string(text, "d.dta", s);
    REQUIRE_FALSE(r);
    return std::string(r.error().message());
}
}  // namespace

TEST_CASE("#pack, #origin and #shape declare, and the tree holds none of them") {
    LoadSession s = leveled();
    const Ref<Array> root =
        ok(read_string("; a pack of the game's\n"
                       "(speed 1)\n"
                       "#origin (\"economy.xlsx\" (tool sheets-export))\n"
                       "#pack (core (level game) (owners a b))\n"
                       "#shape (unit (@at units) (@keyed) (hp int (@range 1 9)))\n"
                       "#shape (item (price int))\n"
                       "(units (grunt (hp 3)))\n",
                       "d.dta", s));
    CHECK(to_string(*root) == "((speed 1) (units (grunt (hp 3))))");
    REQUIRE(s.packs().size() == 1);
    CHECK(s.packs()[0].name == Symbol("core"));
    CHECK(s.packs()[0].file == Symbol("d.dta"));
    REQUIRE(s.origin_of(Symbol("d.dta")));
    CHECK(s.origin_of(Symbol("d.dta"))->source == Symbol("economy.xlsx"));
    REQUIRE(s.shapes().size() == 2);
    CHECK(s.shapes()[0].form->tag() == Symbol("unit"));
    CHECK(s.shapes()[0].line == 5);
    CHECK(s.shapes()[0].root == Symbol("d.dta"));
    CHECK(s.shapes()[1].form->tag() == Symbol("item"));
    // Inside a form, macros expand as in any array; a declaration counts in the branch read.
    LoadSession m = leveled();
    ok(
        read_string("[LEVEL game]\n#ifdef OTHER\n#pack (a (level mod))\n#else\n"
                    "#pack (b (level LEVEL))\n#endif\n",
                    "m.dta", m));
    REQUIRE(m.packs().size() == 1);
    CHECK(m.packs()[0].name == Symbol("b"));
    CHECK(m.packs()[0].level == Symbol("game"));
}

TEST_CASE("the forms that declared by position are data") {
    LoadSession s = leveled();
    const Ref<Array> root = ok(read_string(
        "(pack core (level game))\n(origin \"x.xlsx\")\n(shapes (unit (hp int)))\n", "p.dta", s));
    CHECK(root->size() == 3);
    CHECK(s.packs().empty());
    CHECK_FALSE(s.origin_of(Symbol("p.dta")));
    CHECK(s.shapes().empty());
    // So the printer writes them back as they are, and they read back the same.
    CHECK(to_dta(*root) ==
          "(pack core (level game))\n(origin \"x.xlsx\")\n(shapes (unit (hp int)))\n");
    // A check of a layer that holds them says what each is now, once each, at the form.
    Config cfg({.tweaks = nullptr});
    const LayerReport r = ok(cfg.check_layer_text(
        "old", "(pack core (level game))\n(origin \"x.xlsx\")\n(shapes (unit (hp int)))\n"));
    std::vector<std::string> warnings;
    for (const Error& e : r.warnings) warnings.emplace_back(e.message());
    CHECK(warnings ==
          std::vector<std::string>{
              "(pack ...) is data; a file declares its pack with #pack (NAME (level L) ...)",
              "(origin \"...\") is data; a file declares its origin with #origin (\"source\" ...)",
              "(shapes ...) is data; a file declares each shape with #shape (NAME ...)"});
    CHECK(r.warnings[2].line() == 3);
}

TEST_CASE("a declaration is one form at a file's top level; a pack and an origin once") {
    CHECK(error_of("(a #pack (b (level game)))") == "#pack is written at a file's top level");
    CHECK(error_of("#pack (a (level game))\n#pack (b (level game))") ==
          "second #pack in this file: a file declares one pack");
    CHECK(error_of("#origin (\"a\")\n#origin (\"b\")") ==
          "second #origin in this file: a file declares one origin");
    CHECK(error_of("#pack core") == "#pack takes one form: #pack (NAME (level L) ...)");
    CHECK(error_of("#shape {f}") == "#shape takes one form: #shape (NAME (FIELD TYPE ...) ...)");
    CHECK(error_of("#origin (economy)") ==
          "#origin takes a string first: #origin (\"source\" (tool NAME))");
    CHECK(error_of("#shape ((f int))") ==
          "#shape takes a named form: #shape (NAME (FIELD TYPE ...) ...)");
    CHECK(error_of("#pack ()") == "#pack (...) needs a name");
    CHECK(error_of("#pack (a (level nowhere))").find("not one of the session's pack levels") !=
          std::string::npos);
}

TEST_CASE("a pack is its whole file's: what it defined above #pack is the pack's") {
    LoadSession s = leveled();
    ok(read_string("#pack (base (level game))\n[SPEED 1]\n", "base.dta", s));
    // The mod uses its own macro, defined above its #pack, and the base's: both stand.
    ok(read_string("[BOOST 2]\n#pack (fast (level mod))\n(speed BOOST SPEED)\n", "mod.dta", s));
    CHECK(s.warnings().empty());
    // The base, read again above the mod, uses the mod's macro: the one-way rule warns, at the
    // use, though the use is above the base's #pack.
    LoadSession t = leveled();
    ok(read_string("[BOOST 2]\n#pack (fast (level mod))\n", "mod.dta", t));
    ok(read_string("(speed BOOST)\n#pack (base (level game))\n", "base.dta", t));
    REQUIRE(t.warnings().size() == 1);
    CHECK(std::string(t.warnings()[0].message()).find("uses macro 'BOOST' from the softer pack") !=
          std::string::npos);
    CHECK(t.warnings()[0].line() == 1);
}

TEST_CASE("#define NAME (body) still defines a macro, and names [NAME ...]") {
    LoadSession s;
    const Ref<Array> root = ok(read_string("#define RED (1 0 0)\n(tint RED)\n", "d.dta", s));
    CHECK(to_string(*root) == "((tint 1 0 0))");
    REQUIRE(s.warnings().size() == 1);
    CHECK(std::string(s.warnings()[0].message()) ==
          "#define RED (...) is the older macro form; write [RED ...]");
}

TEST_CASE("a cache keeps a file's #shape declarations") {
    TempDir dir;
    const std::string path = dir.write("s.dta", "#shape (unit (hp int))\n(unit (hp 1))\n");
    LoadSession s;
    s.record_for_cache = true;
    const Ref<Array> root = ok(read_file(path, s));
    std::string bytes;
    REQUIRE(write_cache(*root, string_out(bytes), &s));
    LoadSession back;
    const Ref<Array> cached = ok(read_cache_bytes(bytes, "s.datb", back));
    CHECK(structurally_equal(*root, *cached));
    REQUIRE(back.shapes().size() == 1);
    CHECK(back.shapes()[0].form->tag() == Symbol("unit"));
    CHECK(back.shapes()[0].line == 1);
    CHECK(to_string(*back.shapes()[0].form) == "(unit (hp int))");
}
