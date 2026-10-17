// Directives and macros: include, merge, conditionals, defines, autorun.
#include <doctest.h>
#include <baker/cache.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>
#include <baker/write.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace baker;

namespace {

// A LoadSession over an in-memory file system.
struct Files {
    std::map<std::string, std::string> files;
    std::vector<std::string> reads;
    LoadSession session;

    Files() {
        session.set_reader([this](std::string_view path, TextSink out) {
            reads.emplace_back(path);
            auto it = files.find(std::string(path));
            if (it == files.end()) return false;
            out(it->second);
            return true;
        });
    }
    std::string load(const std::string& path) {
        auto r = read_file(path, session);
        REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : to_string(r.error())));
        return to_string(**r);
    }
    Error fail(const std::string& path) {
        auto r = read_file(path, session);
        REQUIRE(!r.has_value());
        return r.error();
    }
};

std::string text(std::string_view src) {
    auto r = read_string(src, "t.dta");
    REQUIRE_MESSAGE(r.has_value(), (r ? std::string() : to_string(r.error())));
    return to_string(**r);
}
Error error(std::string_view src) {
    auto r = read_string(src, "t.dta");
    REQUIRE(!r.has_value());
    return r.error();
}

}  // namespace

TEST_CASE("macros splice their nodes") {
    CHECK(text("[TRUE 1]\n[RED 1 0 0]\n(flag TRUE) (color RED)") == "((flag 1) (color 1 0 0))");
    CHECK(text("[NOTHING]\n(a NOTHING b)") == "((a b))");
    CHECK(text("[A 1]\n[B A A]\n(x B)") == "((x 1 1))");  // expanded while defining B
    CHECK(text("[X 1]\n(x 'X' X)") == "((x X 1))");       // quoted symbols never expand
    CHECK(text("[A 1]\n[A 2]\n(a A)") == "((a 2))");      // redefinition, name not expanded
    CHECK(text("(TAG 1)\n[TAG t]\n(TAG 2)") == "((TAG 1) (t 2))");  // only after definition
}

TEST_CASE("macro expansion shares arrays") {
    auto root = *read_string("[HANDLER (hit {fall_down})]\n(object1 HANDLER)\n(object2 HANDLER)");
    const Array* h1 = (*(*root)[0].array())[1].array();
    const Array* h2 = (*(*root)[1].array())[1].array();
    CHECK(h1 == h2);
    CHECK(to_string(*root) == "((object1 (hit {fall_down})) (object2 (hit {fall_down})))");
}

TEST_CASE("[ ] defines a macro only at a file's top level") {
    CHECK(text("(a [b c])") == "((a [b c]))");
    CHECK(text("[M [p q]]\n(a M)") == "((a [p q]))");
    CHECK(error("[1 2]").message() == "macro definition needs a symbol name");
    CHECK(error("[]").message() == "macro definition needs a symbol name");
    // A spliced property at top level would print back as a macro definition.
    CHECK(error("[M [p q]]\nM").code() == ErrorCode::Syntax);
}

TEST_CASE("#define and #undef") {
    CHECK(text("#define kMax (4)\n(players kMax)") == "((players 4))");
    CHECK(text("#define PAIR (1 2)\n(p PAIR)") == "((p 1 2))");
    CHECK(text("#define FLAG\n(a FLAG)") == "((a FLAG))");  // a flag is not a macro
    // The body must start on the same line; otherwise the define is a flag.
    CHECK(text("#define FLAG\n(next 1)") == "((next 1))");
    CHECK(text("#define X (1)\n#undef X\n(a X)") == "((a X))");
    CHECK(error("#define").code() == ErrorCode::Directive);
    CHECK(error("#define (a)").message() == "#define needs a name");
    CHECK(error("#undef 5").message() == "#undef needs a name");
}

TEST_CASE("conditionals") {
    CHECK(text("#define A\n#ifdef A (yes) #else (no) #endif") == "((yes))");
    CHECK(text("#ifdef A (yes) #else (no) #endif") == "((no))");
    CHECK(text("#ifndef A (yes) #endif") == "((yes))");
    CHECK(text("[A 1]\n#ifdef A (macro_counts) #endif") == "((macro_counts))");
    // Nested, including inside skipped regions, and spanning brackets.
    CHECK(text("#define A\n"
               "#ifdef A\n"
               "  #ifdef B (ab) #else (a_not_b) #endif\n"
               "#else\n"
               "  #ifdef B (b) #else (neither) #endif\n"
               "#endif") == "((a_not_b))");
    CHECK(text("(list 1\n#ifdef X\n 2)\n#else\n 3)\n#endif") == "((list 1 3))");
    // Skipped regions may contain anything that lexes, including unknown directives.
    CHECK(text("#ifdef NOPE (unbalanced #bogus #include nowhere #endif (ok)") == "((ok))");
    // The name of #ifdef is never macro-expanded.
    CHECK(text("[A B]\n#ifdef A (a) #endif") == "((a))");

    CHECK(error("#else").message() == "#else without #ifdef");
    CHECK(error("#endif").message() == "#endif without #ifdef");
    Error e = error("(a)\n#ifdef X\n(b)");
    CHECK(e.message() == "#ifdef has no matching #endif");
    CHECK(e.line() == 2);
    CHECK(error("#ifdef X #else #else #endif").message() == "second #else for one #ifdef");
    CHECK(error("#ifdef (a) #endif").message() == "#ifdef needs a name");
}

TEST_CASE("unknown directives are errors") {
    Error e = error("(a)\n#bogus x");
    CHECK(e.message() == "unknown directive '#bogus'");
    CHECK(e.code() == ErrorCode::Directive);
    CHECK(e.line() == 2);
}

TEST_CASE("a lone # is a symbol") {
    // Real data binds it as a key: (# {fake_win 3}) in a cheats table.
    auto root = read_string("(# 3) #", "t.dta");
    REQUIRE(root);
    CHECK((*(**root)[0].array())[0].try_sym() == Symbol("#"));
    CHECK((**root)[1].try_sym() == Symbol("#"));
    CHECK(to_dta(**root) == "('#' 3)\n'#'\n");
    auto again = read_string(to_dta(**root), "t.dta");
    REQUIRE(again);
    CHECK(structurally_equal(**root, **again));
}

TEST_CASE("#include splices files, relative to the including file") {
    Files fs;
    fs.files["game/main.dta"] = "(first)\n#include sub/part.dta\n(last)";
    fs.files["game/sub/part.dta"] = "(part 1)\n#include \"leaf.dta\"";
    fs.files["game/sub/leaf.dta"] = "(leaf)";
    CHECK(fs.load("game/main.dta") == "((first) (part 1) (leaf) (last))");

    // Inside an array, the included nodes become children.
    fs.files["in_array.dta"] = "(list 0\n#include items.dta\n 4)";
    fs.files["items.dta"] = "1 2 3";
    CHECK(fs.load("in_array.dta") == "((list 0 1 2 3 4))");
}

TEST_CASE("included arrays carry their own file and line") {
    Files fs;
    fs.files["a.dta"] = "#include b.dta";
    fs.files["b.dta"] = "\n\n(from_b)";
    auto root = *read_file("a.dta", fs.session);
    CHECK((*root)[0].array()->file() == Symbol("b.dta"));
    CHECK((*root)[0].array()->line() == 3);
}

TEST_CASE("macros and defines persist across files in a session") {
    Files fs;
    fs.files["macros.dta"] = "[TRUE 1]\n#define DEBUG";
    fs.files["use.dta"] = "#include macros.dta\n(flag TRUE)\n#ifdef DEBUG (debug) #endif";
    CHECK(fs.load("use.dta") == "((flag 1) (debug))");
    fs.files["later.dta"] = "(still TRUE)";
    CHECK(fs.load("later.dta") == "((still 1))");
    CHECK(fs.session.is_defined("DEBUG"));
    CHECK(fs.session.macro("TRUE") != nullptr);
    CHECK(fs.session.macro("DEBUG") == nullptr);
}

TEST_CASE("include search falls back to include_dirs") {
    Files fs;
    fs.session.add_include_dir("shared");
    fs.session.add_include_dir("other");
    fs.files["levels/l1.dta"] = "#include common.dta";
    fs.files["other/common.dta"] = "(common)";
    CHECK(fs.load("levels/l1.dta") == "((common))");
    CHECK(fs.reads == std::vector<std::string>{"levels/l1.dta", "levels/common.dta",
                                               "shared/common.dta", "other/common.dta"});
}

TEST_CASE("include errors") {
    Files fs;
    fs.files["a.dta"] = "(a)\n#include missing.dta";
    Error e = fs.fail("a.dta");
    CHECK(e.code() == ErrorCode::Io);
    CHECK(e.message() == "cannot read included file 'missing.dta'");
    CHECK(e.file() == Symbol("a.dta"));
    CHECK(e.line() == 2);

    fs.files["opt.dta"] = "(a)\n#include_opt missing.dta\n(b)";
    CHECK(fs.load("opt.dta") == "((a) (b))");

    fs.files["self.dta"] = "#include self.dta";
    CHECK(fs.fail("self.dta").message() == "'self.dta' includes itself");
    fs.files["x.dta"] = "#include y.dta";
    fs.files["y.dta"] = "#include x.dta";
    CHECK(fs.fail("x.dta").code() == ErrorCode::Directive);

    fs.files["bad_inner.dta"] = "#include broken.dta";
    fs.files["broken.dta"] = "\n(oops";
    e = fs.fail("bad_inner.dta");
    CHECK(e.file() == Symbol("broken.dta"));
    CHECK(e.line() == 2);

    fs.files["noarg.dta"] = "#include (x)";
    CHECK(fs.fail("noarg.dta").message() == "#include needs a file path");

    // An include-only-once loop that is not a cycle still hits the include budget.
    fs.session.limits.max_includes = 10;
    fs.files["many.dta"] =
        "#include leaf.dta\n#include leaf.dta\n#include leaf.dta\n"
        "#include leaf.dta\n#include leaf.dta\n#include leaf.dta\n"
        "#include leaf.dta\n#include leaf.dta\n#include leaf.dta\n"
        "#include leaf.dta\n#include leaf.dta";
    fs.files["leaf.dta"] = "(leaf)";
    CHECK(fs.fail("many.dta").code() == ErrorCode::Limit);
}

TEST_CASE("#merge keeps existing values and adds missing ones") {
    Files fs;
    // The GDC slide example: existing leaves win, missing arrays are added.
    fs.files["slide.dta"] = "(a (b 1))\n#merge more.dta";
    fs.files["more.dta"] = "(a (b 2) (c 2))";
    CHECK(fs.load("slide.dta") == "((a (b 1) (c 2)))");

    fs.files["app.dta"] =
        "(renderer (screen_size 1920 1080) (shadows (resolution 4096)))\n"
        "#merge default.dta";
    fs.files["default.dta"] =
        "(renderer (screen_size 1280 720) (vsync 1) (shadows (enabled 1) (resolution 2048)))\n"
        "(audio (channels 64))\n"
        "loose_symbol\n"
        "(1 2 3)";
    CHECK(fs.load("app.dta") ==
          "((renderer (screen_size 1920 1080) (shadows (resolution 4096) (enabled 1)) (vsync 1)) "
          "(audio (channels 64)) (1 2 3))");

    // Inside an array, #merge merges into that array.
    fs.files["inner.dta"] = "(cfg (a 1)\n#merge extra.dta\n)";
    fs.files["extra.dta"] = "(a 9) (b 2)";
    CHECK(fs.load("inner.dta") == "((cfg (a 1) (b 2)))");
}

TEST_CASE("#merge never mutates shared arrays") {
    Files fs;
    fs.files["m.dta"] = "[BASE (cfg (a 1))]\n(one BASE)\n(two BASE\n#merge add.dta\n)";
    fs.files["add.dta"] = "(cfg (b 2))";
    CHECK(fs.load("m.dta") == "((one (cfg (a 1))) (two (cfg (a 1) (b 2))))");
}

TEST_CASE("#autorun collects commands") {
    LoadSession s;
    auto r = read_string("(a)\n#autorun {setup 1}\n(b)\n#autorun {setup 2}", "t.dta", s);
    REQUIRE(r);
    CHECK(to_string(**r) == "((a) (b))");
    REQUIRE(s.autorun().size() == 2);
    CHECK(s.autorun()[0].type() == Type::Command);
    CHECK(to_string(s.autorun()[1]) == "{setup 2}");
    CHECK(error("#autorun (x)").message() == "#autorun needs a {command}");
}

TEST_CASE("an included file's nodes count once toward max_nodes") {
    Files fs;
    fs.files["inc.dta"] = "1 2 3 4 5 6 7 8 9 10\n";  // 10 nodes
    fs.files["main.dta"] = "#include inc.dta\n";
    fs.session.limits.max_nodes = 10;
    CHECK(fs.load("main.dta") == "(1 2 3 4 5 6 7 8 9 10)");
    fs.session.limits.max_nodes = 9;
    CHECK(fs.fail("main.dta").code() == ErrorCode::Limit);
}

TEST_CASE("size limits stop macro blow-ups") {
    LoadSession s;
    s.limits.max_nodes = 10000;
    std::string src = "[A x x]\n";
    char prev = 'A';
    for (char c = 'B'; c <= 'Z'; ++c, ++prev)
        src += std::string("[") + c + ' ' + prev + ' ' + prev + "]\n";
    src += "(boom Z)";
    auto r = read_string(src, "bomb.dta", s);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code() == ErrorCode::Limit);

    // Shared-array chains stay small in memory but their height is still bounded.
    std::string chain = "[M0 (x)]\n";
    for (int i = 1; i <= 300; ++i)
        chain += "[M" + std::to_string(i) + " (M" + std::to_string(i - 1) + ")]\n";
    auto deep = read_string(chain);
    REQUIRE_FALSE(deep.has_value());
    CHECK(deep.error().code() == ErrorCode::Limit);
}

TEST_CASE("#require reads a file once per session") {
    Files fs;
    // A pack: a macro, and a node that shows each time the pack is read.
    fs.files["pack.dta"] = "#define SPEED (speed 3)\n(pack_read)";
    for (const char* f : {"a.dta", "b.dta", "c.dta"})
        fs.files[f] = std::string("#require pack.dta\n(") + f[0] + " SPEED)";
    fs.files["main.dta"] = "#include a.dta\n#include b.dta\n#include c.dta";
    // The pack is read once, from three files, and its macro serves all three.
    CHECK(fs.load("main.dta") == "((pack_read) (a speed 3) (b speed 3) (c speed 3))");
    CHECK(std::count(fs.reads.begin(), fs.reads.end(), "pack.dta") == 1);

    // Later top-level reads through the session skip it too; #include still splices it.
    CHECK(fs.load("a.dta") == "((a speed 3))");
    fs.files["d.dta"] = "#include pack.dta";
    CHECK(fs.load("d.dta") == "((pack_read))");
    // A copied session keeps the set; a fresh one does not.
    LoadSession copy = fs.session;
    CHECK(to_string(**read_file("a.dta", copy)) == "((a speed 3))");
    Files fresh;
    fresh.files = fs.files;
    CHECK(fresh.load("a.dta") == "((pack_read) (a speed 3))");
    // A missing required file is an error, as for #include.
    fs.files["e.dta"] = "#require nope.dta";
    CHECK(fs.fail("e.dta").code() == ErrorCode::Io);
}

TEST_CASE("a pack whose required pack is not loaded is warned about") {
    auto requires_warnings = [](const LoadSession& s) {
        std::vector<std::string> out;
        for (const Error& w : s.warnings())
            if (std::string(w.message()).find(" requires ") != std::string::npos)
                out.push_back(std::string(w.file().str()) + ":" + std::to_string(w.line()) + ": " +
                              std::string(w.message()));
        return out;
    };
    Files fs;
    fs.session.set_pack_levels({"game", "dlc"});
    fs.files["combat_core.dta"] = "(pack combat_core (level game))\n#define HIT (2)";
    // The cookbook's pattern: declare what it requires, then #require it.
    fs.files["boss_tools.dta"] =
        "(pack boss_tools (level dlc) (requires combat_core))\n#require combat_core.dta\n"
        "(boss (hit HIT))";
    fs.files["expansion.dta"] =
        "; an expansion\n(pack expansion (level dlc) (requires season_pass))\n"
        "(zone (desert 4))";
    CHECK(fs.load("boss_tools.dta") == "((boss (hit 2)))");
    CHECK(requires_warnings(fs.session).empty());
    CHECK(fs.load("expansion.dta") == "((zone (desert 4)))");  // loads, and is warned about
    const auto warned = requires_warnings(fs.session);
    REQUIRE(warned.size() == 1);
    CHECK(warned[0].find("pack 'expansion' requires 'season_pass', which is not loaded") !=
          std::string::npos);
    CHECK(warned[0].find("expansion.dta:2") != std::string::npos);

    // A cache replays the packs and checks them once all are back.
    for (const char* file : {"boss_tools.dta", "expansion.dta"}) {
        LoadSession bake;
        bake.set_pack_levels({"game", "dlc"});
        bake.set_reader(fs.session.reader());
        bake.record_for_cache = true;
        auto root = read_file(file, bake);
        REQUIRE(root);
        std::string bytes;
        REQUIRE(write_cache(**root, string_out(bytes), &bake));
        LoadSession game;
        game.set_pack_levels({"game", "dlc"});
        REQUIRE(read_cache_bytes(bytes, file, game));
        CHECK(requires_warnings(game).size() == (std::string(file) == "expansion.dta" ? 1u : 0u));
    }
}

TEST_CASE("packs declare versions, and a required version is checked") {
    auto warnings_of = [](const LoadSession& s) {
        std::vector<std::string> out;
        for (const Error& w : s.warnings()) out.emplace_back(w.message());
        return out;
    };
    auto session = [](Files& fs) { fs.session.set_pack_levels({"game", "dlc"}); };
    {
        Files fs;
        session(fs);
        fs.files["base.dta"] = "(pack base (level game) (version \"1.2\"))\n(a 1)";
        fs.files["ok.dta"] =
            "(pack ok_dlc (level dlc) (requires (base \"1.2\")))\n#require base.dta";
        fs.files["new.dta"] = "(pack new_dlc (level dlc) (requires (base \"1.10\")))\n";
        fs.load("ok.dta");
        CHECK(warnings_of(fs.session).empty());
        REQUIRE(fs.session.packs().size() == 2);
        CHECK(fs.session.packs()[0].requires_versions[0] == Symbol("1.2"));  // ok_dlc, first
        CHECK(fs.session.packs()[1].version == Symbol("1.2"));               // base, required
        fs.load("new.dta");  // 1.10 is after 1.2: part by part, not as text
        const auto w = warnings_of(fs.session);
        REQUIRE(w.size() == 1);
        CHECK(w[0] == "pack 'new_dlc' requires 'base' \"1.10\" or later, which is \"1.2\"");
    }
    {
        Files fs;
        session(fs);
        fs.files["base.dta"] = "(pack base (level game) (version 2))";  // an int is a version
        fs.files["plain.dta"] = "(pack plain (level game))";
        fs.files["dlc.dta"] =
            "(pack dlc (level dlc) (requires (base \"1.9.3\") (plain \"1\")))\n"
            "#require base.dta\n#require plain.dta";
        fs.load("dlc.dta");
        const auto w = warnings_of(fs.session);
        REQUIRE(w.size() == 1);
        CHECK(w[0] == "pack 'dlc' requires 'plain' \"1\" or later, which declares no version");
    }
    for (const char* bad :
         {"(pack p (level game) (version 1.10))", "(pack p (level game) (version \"1.x\"))",
          "(pack p (level game) (requires (base 1.5)))",
          "(pack p (level game) (requires (\"1.2\")))"}) {
        Files fs;
        session(fs);
        fs.files["bad.dta"] = bad;
        fs.fail("bad.dta");
    }
    // A cache keeps the versions and checks them on replay.
    Files fs;
    session(fs);
    fs.files["new.dta"] = "(pack new_dlc (level dlc) (version \"3\") (requires (base \"1.10\")))";
    LoadSession bake;
    bake.set_pack_levels({"game", "dlc"});
    bake.set_reader(fs.session.reader());
    bake.record_for_cache = true;
    auto root = read_file("new.dta", bake);
    REQUIRE(root);
    std::string bytes;
    REQUIRE(write_cache(**root, string_out(bytes), &bake));
    LoadSession game;
    game.set_pack_levels({"game", "dlc"});
    REQUIRE(read_cache_bytes(bytes, "new.datb", game));
    REQUIRE(game.packs().size() == 1);
    CHECK(game.packs()[0].version == Symbol("3"));
    CHECK(game.packs()[0].requires_versions[0] == Symbol("1.10"));
    CHECK(warnings_of(game).size() == 1);  // base is not loaded
}

TEST_CASE("packs declare a level, and the one-way rule warns on softer macros") {
    Files fs;
    fs.session.set_pack_levels({"engine", "game", "task"});
    fs.files["engine.dta"] = "(pack engine_core (level engine))\n#define GRAVITY (9.8)";
    fs.files["task.dta"] =
        "(pack boss_task (level task) (requires game_rules))\n#define PHASES (3)\n"
        "(uses GRAVITY)";
    fs.files["game.dta"] =
        "(pack game_rules (level game))\n#require engine.dta\n#require task.dta\n"
        "(a GRAVITY PHASES)\n(b PHASES)";
    fs.files["loose.dta"] = "#require task.dta\n(free PHASES)";

    // The declarations are recorded and left out of the tree.
    CHECK(fs.load("game.dta") == "((uses 9.8) (a 9.8 3) (b 3))");
    REQUIRE(fs.session.packs().size() == 3);
    const Pack* game = fs.session.pack_of("game.dta");
    REQUIRE(game);
    CHECK(game->name == Symbol("game_rules"));
    CHECK(game->rank == 1);
    const Pack* task = fs.session.pack_of("task.dta");
    REQUIRE(task);
    CHECK(task->rank == 2);
    REQUIRE(task->requires_packs.size() == 1);
    CHECK(task->requires_packs[0] == Symbol("game_rules"));

    // The game pack used the task pack's macro, twice, on line 4: one warning, located.
    // The task pack using the engine's GRAVITY is fine: softer may use harder.
    REQUIRE(fs.session.warnings().size() == 1);
    const Error& w = fs.session.warnings()[0];
    CHECK(w.file() == Symbol("game.dta"));
    CHECK(w.line() == 4);
    CHECK(to_string(w).find("pack 'game_rules' (level game) uses macro 'PHASES' from the "
                            "softer pack 'boss_task' (level task)") != std::string::npos);
    // A file that declares no pack is unconstrained.
    fs.session.clear_warnings();
    CHECK(fs.load("loose.dta") == "((free 3))");
    CHECK(fs.session.warnings().empty());
}

TEST_CASE("packs carry harness metadata: owners, audience, stage") {
    Files fs;
    fs.session.set_pack_levels({"game", "task"});
    fs.files["boss.dta"] =
        "(pack boss (level task) (requires combat)\n"
        "      (owners \"td-encounter\" td-combat) (for designers testers) (stage custom))\n"
        "(x 1)";
    CHECK(fs.load("boss.dta") == "((x 1))");
    const Pack* p = fs.session.pack_of(Symbol("boss.dta"));
    REQUIRE(p);
    REQUIRE(p->owners.size() == 2);
    CHECK(p->owners[0] == Symbol("td-encounter"));
    CHECK(p->owners[1] == Symbol("td-combat"));
    REQUIRE(p->audience.size() == 2);
    CHECK(p->audience[1] == Symbol("testers"));
    CHECK(p->stage == Symbol("custom"));
    // None declared: empty.
    fs.files["plain.dta"] = "(pack plain (level game))";
    (void)fs.load("plain.dta");
    const Pack* q = fs.session.pack_of(Symbol("plain.dta"));
    REQUIRE(q);
    CHECK(q->owners.empty());
    CHECK(q->audience.empty());
    CHECK(q->stage.empty());

    fs.files["bad.dta"] = "(pack bad (level game) (stage finished))";
    CHECK(to_string(fs.fail("bad.dta")) ==
          "baker: pack 'bad': (stage S) is genesis, custom, product, or commodity  (bad.dta:1)");
    fs.files["bad2.dta"] = "(pack bad2 (level game)\n(owners \"a\" 3))";
    CHECK(to_string(fs.fail("bad2.dta")) ==
          "baker: pack 'bad2': (owners ...) lists names  (bad2.dta:2)");
}

TEST_CASE("a file's origin is recorded and left out of the tree") {
    Files fs;
    fs.session.set_pack_levels({"game"});
    fs.files["econ.dta"] =
        "; generated: do not edit\n"
        "(origin \"economy.xlsx\" (tool sheets-export))\n"
        "(econ (price 5))";
    CHECK(fs.load("econ.dta") == "((econ (price 5)))");
    const Origin* o = fs.session.origin_of(Symbol("econ.dta"));
    REQUIRE(o);
    CHECK(o->source == Symbol("economy.xlsx"));
    CHECK(o->tool == Symbol("sheets-export"));
    CHECK(fs.session.origins().size() == 1);

    // After a pack declaration too; the tool is optional.
    fs.files["p.dta"] = "(pack p (level game))\n(origin \"levels.csv\")\n(a 1)";
    CHECK(fs.load("p.dta") == "((a 1))");
    REQUIRE(fs.session.origin_of(Symbol("p.dta")));
    CHECK(fs.session.origin_of(Symbol("p.dta"))->tool.empty());

    // An included file declares its own.
    fs.files["main.dta"] = "(b 2)\n#include econ.dta";
    CHECK(fs.load("main.dta") == "((b 2) (econ (price 5)))");
    CHECK_FALSE(fs.session.origin_of(Symbol("main.dta")));

    // Ordinary data is not taken for one: a value that is not a string, a later form, a
    // second origin.
    fs.files["d.dta"] = "(origin 0 0 0)";
    CHECK(fs.load("d.dta") == "((origin 0 0 0))");
    fs.files["e.dta"] = "(x 1)\n(origin \"a.xlsx\")";
    CHECK(fs.load("e.dta") == "((x 1) (origin \"a.xlsx\"))");
    fs.files["f.dta"] = "(origin \"a.xlsx\")\n(origin \"b.xlsx\")";
    CHECK(fs.load("f.dta") == "((origin \"b.xlsx\"))");
    CHECK(fs.session.origin_of(Symbol("f.dta"))->source == Symbol("a.xlsx"));
    CHECK_FALSE(fs.session.origin_of(Symbol("d.dta")));

    fs.files["g.dta"] = "(origin \"a.xlsx\" (tool))";
    CHECK(to_string(fs.fail("g.dta")) == "baker: write (origin \"source\" (tool name))  (g.dta:1)");
}

TEST_CASE("a pack's level must be one of the session's") {
    Files fs;
    fs.files["p.dta"] = "(pack p (level nowhere))";
    CHECK(to_string(fs.fail("p.dta")).find("not one of the session's pack levels ()") !=
          std::string::npos);
    fs.session.set_pack_levels({"engine"});
    CHECK(to_string(fs.fail("p.dta")).find("(engine)") != std::string::npos);
    fs.files["q.dta"] = "(pack q)";
    CHECK(to_string(fs.fail("q.dta")).find("needs a (level L)") != std::string::npos);
    // Only the first form declares: a later (pack ...) is data.
    fs.files["r.dta"] = "(x 1)\n(pack r (level nowhere))";
    CHECK(fs.load("r.dta") == "((x 1) (pack r (level nowhere)))");
}

TEST_CASE("a session lists the macro definitions its reads made, where each is written") {
    Files f;
    f.files["main.dta"] =
        "#define PC\n"
        "[SPEED 3]\n"
        "#include colors.dta\n"
        "#define BIG (scale 2)\n"
        "[SPEED 4]\n"
        "#undef PC\n"
        "(speed SPEED)\n";
    f.files["colors.dta"] = "\n[RED 1 0 0]\n";
    f.session.define(Symbol("HOST"));  // defined in code: not the data's
    f.load("main.dta");
    std::vector<std::string> got;
    for (const Definition& d : f.session.macro_definitions())
        got.push_back(std::string(d.name.str()) + "@" + std::string(d.file.str()) + ":" +
                      std::to_string(d.line) + (d.flag ? " flag" : ""));
    CHECK(got == std::vector<std::string>{"PC@main.dta:1 flag", "SPEED@main.dta:2",
                                          "RED@colors.dta:2", "BIG@main.dta:4",
                                          "SPEED@main.dta:5"});
    CHECK_FALSE(f.session.is_defined(Symbol("PC")));  // listed, and #undef'd

    // A load replayed from its cache lists the same definitions.
    LoadSession bake;
    bake.set_reader(f.session.reader());
    bake.record_for_cache = true;
    auto root = read_file("main.dta", bake);
    REQUIRE(root);
    std::string bytes;
    REQUIRE(write_cache(**root, string_out(bytes), &bake));
    LoadSession game;
    REQUIRE(read_cache_bytes(bytes, "main.datb", game));
    REQUIRE(game.macro_definitions().size() == 5);
    CHECK(game.macro_definitions()[2].name == Symbol("RED"));
    CHECK(game.macro_definitions()[2].file == Symbol("colors.dta"));
    CHECK(game.macro_definitions()[2].line == 2);
    CHECK(game.macro_definitions()[0].flag);
}
