// Rows keyed by a path: (/a.b.c 1) is (a (b (c 1))).
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/source_map.hpp>
#include <baker/std_interop.hpp>
#include <baker/write.hpp>

#include <string>
#include <vector>
#include "dev_only.hpp"
#include "ok.hpp"
#include "temp_dir.hpp"

using namespace baker;
using baker::test::ok;
using baker::test::TempDir;

namespace {
std::string nested(std::string_view text) {
    return to_dta(*ok(read_string(text, "r.dta")));
}
std::string error_of(std::string_view text) {
    auto r = read_string(text, "r.dta");
    REQUIRE(!r);
    return std::string(r.error().message());
}
}  // namespace

TEST_CASE("a row is the nested tree its key names, folded with the file's other rows") {
    CHECK(nested("(/fighters.kestrel.health 1100)\n(/fighters.kestrel.walk_back 1.7)\n"
                 "(/cpu.easy.reaction 26)\n") ==
          "(fighters (kestrel (health 1100) (walk_back 1.7)))\n(cpu (easy (reaction 26)))\n");
    CHECK(nested("(/a/b/c 1)\n") == "(a (b (c 1)))\n");  // either separator, as Path reads
    CHECK(nested("(/menu.'v1.2'.price 3)\n") == "(menu (v1.2 (price 3)))\n");
    CHECK(nested("(/menu.debug_menu #remove)\n") == "(menu (debug_menu #remove))\n");
    CHECK(nested("(/a.b)\n") == "(a (b))\n");
    // A row and a nested array of one prefix fold too.
    CHECK(nested("(box (height 3))\n(/box.width 4)\n") == "(box (height 3) (width 4))\n");
    // The one value run comes first, whichever row wrote it: never after metadata.
    const Ref<Array> root = ok(read_string("(/a.b (@range 0 9))\n(/a.b 5)\n", "r.dta"));
    const Array& b = *root->try_get("a.b");
    REQUIRE(b.values().size() == 1);
    CHECK(b.values()[0].try_int() == 5);
    CHECK(b.meta("@range"));
}

TEST_CASE("what is not a row stays as it is") {
    CHECK(nested("('/x' 1)\n") == "('/x' 1)\n");  // quoted: a symbol
    CHECK(nested("(path /usr/share)\n") == "(path /usr/share)\n");
    CHECK(nested("(/ 1 2)\n") == "(/ 1 2)\n");  // a lone '/'
    CHECK(nested("{/a.b 1}\n") == "{/a.b 1}\n");
    // A file without rows keeps first-wins for a repeated tag.
    CHECK(nested("(x 1)\n(x 2)\n") == "(x 1)\n(x 2)\n");
    // The printer quotes a '/'-led head, so the text reads back as the same tree.
    const Ref<Array> odd = array(array(Symbol("/x"), 1));
    CHECK(to_dta(*odd) == "('/x' 1)\n");
}

TEST_CASE("a row's key errors are located at the key") {
    CHECK(error_of("(/fighters.0.health 5)\n").find("'0' is an element") != std::string::npos);
    CHECK(error_of("(/a..b 1)\n") == "the key has an empty segment");
    CHECK(error_of("(/a.b/ 1)\n") == "the key ends with a separator");
    CHECK(error_of("(/a.'b 1)\n").find("unclosed quote") != std::string::npos);
    CHECK(error_of("(/a.1e5 1)\n") == "the key's segment 1e5 must be quoted: '1e5'");
    CHECK(error_of("(a (/b 1))\n").find("a row is a top-level form") != std::string::npos);
    std::string deep = "(/a";
    for (int i = 0; i < 16; ++i) deep += ".a";
    CHECK(error_of(deep + " 1)\n") == "the key has more than 16 segments");
    auto r = read_string("(x 1)\n   (/a.b 1)\n", "r.dta");
    REQUIRE(r);
    r = read_string("(/a.b 1)\n  (/a.b 2)\n", "r.dta");
    REQUIRE(!r);
    CHECK(r.error().message() == "'a/b' is written twice");
    CHECK(r.error().line() == 2);
    CHECK(r.error().column() == 4);  // the key's first byte
    CHECK(error_of("(/a.b #remove)\n(/a.b.c 1)\n") == "'a/b' is removed and also written");
}

TEST_CASE("a row in a list of records names the cause") {
    // A file of rows holds each key once; a list repeats a tag.
    auto r = read_string("(scenario a (seconds 1))\n(scenario b (seconds 2))\n(/x 1)\n", "r.dta");
    REQUIRE(!r);
    CHECK(r.error().line() == 2);
    CHECK(std::string(r.error().message()) ==
          "'scenario' is written twice; this file holds rows (the first at line 3), so each key "
          "is written once: a list of records is written without rows");
}

TEST_CASE("rows with macros, conditionals, includes and declarations") {
    CHECK(nested("[V 7]\n(/a.b V)\n") == "(a (b 7))\n");  // the body expands
    CHECK(nested("[a 7]\n(/a.b 1)\n") == "(a (b 1))\n");  // the key never does
    CHECK(error_of("[M (/a 1)]\n").find("a row is a top-level form") != std::string::npos);
    CHECK(nested("#define X\n#ifdef X\n(/a.b 1)\n#else\n(/a.b 2)\n#endif\n") == "(a (b 1))\n");
    // A row never declares: (/pack ...) is data, as (pack ...) is since #pack.
    const Ref<Array> pack = ok(read_string("(/pack drops (level live))\n(a 1)\n", "r.dta"));
    CHECK(pack->size() == 2);
    CHECK(to_dta(*pack) == "(pack drops (level live))\n(a 1)\n");
    CHECK(structurally_equal(*ok(read_string(to_dta(*pack), "p.dta")), *pack));

    TempDir dir;
    dir.write("rows.dta", "(/c.d 2)\n(/c.e 3)\n");
    const Ref<Array> inc = ok(read_file(dir.write("main.dta", "(a #include rows.dta)\n")));
    CHECK(to_dta(*inc) == "(a (c (d 2) (e 3)))\n");  // folded in its file, spliced in place
}

TEST_CASE("edit mode: per-segment spans, and the arrays a key made") {
    const std::string text = "(/a.b.c 5)  ; tuned\n";
    LoadSession s;
    SourceMap map;
    s.source_map = &map;
    const Ref<Array> root = ok(read_string(text, "r.dta", s));
    const Array& a = *root->try_find("a");
    const Array& b = *a.try_find("b");
    const Array& c = *b.try_find("c");
    CHECK(map.row_made(a));
    CHECK(map.row_made(b));
    CHECK(!map.row_made(c));  // the row's own array: its brackets are the row's
    auto at = [&](const Array& x, size_t i) {
        const SourceSpan* sp = map.span(x, i);
        REQUIRE(sp);
        return text.substr(sp->begin, sp->end - sp->begin);
    };
    CHECK(at(a, 0) == "a");
    CHECK(at(b, 0) == "b");
    CHECK(at(c, 0) == "c");
    CHECK(at(c, 1) == "5");
    CHECK(at(b, 1) == "(/a.b.c 5)");
}

BAKER_DEV_TEST_CASE("write-back into a file of rows") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", dir.write("default.dta", "(box (width 2) (height 1))\n")));
    REQUIRE(cfg.push_layer(
        "agent", dir.write("agent.dta", "(/box.width 3)  ; wider\n(/cpu.easy.reaction 26)\n")));
    CHECK(cfg.source("cpu.easy.reaction")->line == 2);

    // A value: its token, the comment kept.
    REQUIRE(cfg.set("box.width", Node(4)));
    REQUIRE(cfg.write_back("box.width"));
    CHECK(dir.read("agent.dta") == "(/box.width 4)  ; wider\n(/cpu.easy.reaction 26)\n");

    // A key the layer lacks: a row beside its siblings, not at the file's end.
    REQUIRE(cfg.set("box.height", Node(2)));
    REQUIRE(cfg.write_back_to("box.height", "agent"));
    CHECK(dir.read("agent.dta") ==
          "(/box.width 4)  ; wider\n(/box.height 2)\n(/cpu.easy.reaction 26)\n");
    // One sharing no prefix: at the end.
    REQUIRE(cfg.set("audio.volume", Node(0.5)));
    REQUIRE(cfg.write_back_to("audio.volume", "agent"));
    CHECK(dir.read("agent.dta") ==
          "(/box.width 4)  ; wider\n(/box.height 2)\n"
          "(/cpu.easy.reaction 26)\n(/audio.volume 0.5)\n");
    CHECK(cfg.get<double>("audio.volume") == doctest::Approx(0.5));

    // A nested file stays nested.
    REQUIRE(cfg.set("box.depth", Node(7)));
    REQUIRE(cfg.write_back_to("box.depth", "default"));
    CHECK(dir.read("default.dta").find("(depth 7)") != std::string::npos);
}

BAKER_DEV_TEST_CASE("values for a key a row's key made go in a row of their own") {
    TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("agent", dir.write("agent.dta", "(/a.b.c 1)\n")));
    REQUIRE(cfg.set("a.b", Node(5)));
    REQUIRE(cfg.write_back_to("a.b", "agent"));
    CHECK(dir.read("agent.dta") == "(/a.b.c 1)\n(/a.b 5)\n");
    CHECK(cfg.get<int>("a.b") == 5);
    CHECK(cfg.get<int>("a.b.c") == 1);
}

BAKER_DEV_TEST_CASE("promote carries a removal, and a rows layer promotes as a nested one") {
    TempDir dir;
    auto stack = [&](const std::string& agent) {
        auto cfg = std::make_unique<Config>(ConfigOptions{.tweaks = nullptr});
        REQUIRE(cfg->push_layer("default", dir.write("default.dta",
                                                     "(menu\n   (play 1)\n   (debug_menu 1))\n"
                                                     "(player (speed 3.5))\n")));
        REQUIRE(cfg->push_layer("agent", dir.write("agent.dta", agent)));
        return cfg;
    };
    auto cfg = stack("(/menu.debug_menu #remove)\n(/player.speed 4.5)\n");
    const auto r = cfg->promote("agent", "default");
    REQUIRE(r);
    CHECK(r->refused.empty());
    CHECK(dir.read("default.dta") == "(menu\n   (play 1))\n(player (speed 4.5))\n");

    // The same layer written nested promotes to the same file.
    const std::string as_rows = dir.read("default.dta");
    dir.write("default.dta", "(menu\n   (play 1)\n   (debug_menu 1))\n(player (speed 3.5))\n");
    cfg = stack("(menu (debug_menu #remove))\n(player (speed 4.5))\n");
    REQUIRE(cfg->promote("agent", "default"));
    CHECK(dir.read("default.dta") == as_rows);
}

TEST_CASE("a layer's effect and source see every row of it") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", ok(read_string("(fighters (kestrel (health 1000)))\n"
                                                     "(cpu (easy (reaction 30)))\n",
                                                     "default.dta"))));
    const char* agent =
        "(/fighters.kestrel.health 1100)\n(/fighters.kestrel.walk_back 1.7)\n"
        "(/cpu.easy.reaction 26)\n";
    const auto report = cfg.check_layer_text("agent", agent);
    REQUIRE(report);
    CHECK(report->effect.size() == 3);  // folded: no row hides behind another of its prefix
    REQUIRE(cfg.push_layer("agent", ok(read_string(agent, "agent.dta"))));
    CHECK(cfg.source("fighters.kestrel.walk_back")->line == 2);
}

TEST_CASE("a received layer's rows are held to its policy at the row's line") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer("default", ok(read_string("(a (b 1))\n", "default.dta"))));
    const auto report =
        cfg.check_layer_text("overlay", "(/a.b 2)\n(/a.c {spawn 1})\n", {.data_only = true});
    REQUIRE(report);
    REQUIRE(report->refusals.size() == 1);
    CHECK(report->refusals[0].line() == 2);
}

namespace {
// Bytes live from one allocator.
struct LiveBytes {
    size_t live = 0;
    baker::Allocator allocator() {
        return {[](void* u, size_t size, size_t, baker::MemoryKind) -> void* {
                    static_cast<LiveBytes*>(u)->live += size;
                    return ::operator new(size);
                },
                [](void* u, void* p, size_t size, baker::MemoryKind) {
                    static_cast<LiveBytes*>(u)->live -= size;
                    ::operator delete(p);
                },
                this};
    }
};
}  // namespace

TEST_CASE("a file of rows holds about what the same file written nested holds") {
    std::string rows, nested = "(world";
    for (int i = 0; i < 500; ++i) {
        const std::string n = std::to_string(i);
        rows += "(/world.zone" + n + ".spawn.rate " + n + ")\n";
        nested += " (zone" + n + " (spawn (rate " + n + ")))";
    }
    nested += ")\n";
    auto held = [](const std::string& text) {
        LiveBytes bytes;
        const Allocator alloc = bytes.allocator();
        LoadSession s;
        s.allocator = &alloc;
        const auto root = read_string(text, "w.dta", s);
        REQUIRE(root);
        return std::pair{bytes.live, to_string(**root)};
    };
    const auto [rows_bytes, rows_text] = held(rows);
    const auto [nested_bytes, nested_text] = held(nested);
    CHECK((rows_text == nested_text));
    MESSAGE("rows " << rows_bytes << " B, nested " << nested_bytes << " B");
    CHECK(rows_bytes <= nested_bytes * 5 / 4);  // 2.3 times before
}
