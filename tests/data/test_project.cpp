// Project files.
#include <doctest.h>
#include <baker/project.hpp>
#include <baker/std_interop.hpp>

#include "temp_dir.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;
using baker::test::TempDir;

namespace {

const char* const kProject =
    "; the stack the game loads\n"
    "(project\n"
    "   (pack_levels engine game task)\n"
    "   (include_dirs \"data/shared\")\n"
    "   (defines PC DEMO)\n"
    "   (reference \"build/reference.json\")\n"
    "   (layers\n"
    "      (default \"data/default.dta\")\n"
    "      (local \"data/local.dta\" optional)\n"
    "      (experiment \"data/experiment.dta\" optional disabled)))\n";

}  // namespace

TEST_CASE("read_project reads the layers, levels, include dirs, and defines") {
    TempDir dir;
    const std::string file = dir.write("baker-project.dta", kProject);
    const Project p = ok(read_project(file));
    CHECK(p.file == Symbol(file));
    REQUIRE(p.layers.size() == 3);
    CHECK(p.layers[0].name == Symbol("default"));
    CHECK(p.layers[0].path == Symbol(dir.path("data/default.dta")));
    CHECK_FALSE(p.layers[0].optional);
    CHECK(p.layers[1].optional);
    CHECK(p.layers[2].optional);
    CHECK_FALSE(p.layers[2].enabled);
    REQUIRE(p.pack_levels.size() == 3);
    CHECK(p.pack_levels[1].value == Symbol("game"));
    REQUIRE(p.include_dirs.size() == 1);
    CHECK(p.include_dirs[0].value == Symbol(dir.path("data/shared")));
    REQUIRE(p.defines.size() == 2);
    CHECK(p.defines[0].value == Symbol("PC"));
    CHECK(p.reference == Symbol(dir.path("build/reference.json")));  // not read, need not exist
}

TEST_CASE("each entry of a project file keeps its line and column") {
    TempDir dir;
    const Project p = ok(read_project(dir.write("baker-project.dta", kProject)));
    auto at = [](ProjectPosition pos, int line, int column) {
        return pos.line == line && pos.column == column;
    };
    CHECK(at(p.pack_levels[0].at, 3, 17));
    CHECK(at(p.pack_levels[2].at, 3, 29));
    CHECK(at(p.include_dirs[0].at, 4, 18));
    CHECK(at(p.defines[1].at, 5, 16));
    CHECK(at(p.reference_at, 6, 15));
    CHECK(at(p.layers[0].name_at, 8, 8));
    CHECK(at(p.layers[0].path_at, 8, 16));
    CHECK(at(p.layers[2].path_at, 10, 19));

    // Columns count characters, and a byte-order mark is not one.
    const Project q = ok(read_project(dir.write(
        "other.dta", "\xEF\xBB\xBF(project (layers (d\xC3\xA9j\xC3\xA0 \"v\xC3\xBC.dta\")))")));
    CHECK(at(q.layers[0].name_at, 1, 19));
    CHECK(at(q.layers[0].path_at, 1, 24));
}

TEST_CASE("find_project finds the nearest project file at or above a path") {
    TempDir dir;
    dir.write("games/a/baker-project.dta", "(project (layers))");
    dir.write("games/a/data/x.dta", "");
    dir.write("baker-project.dta", "(project (layers))");
    CHECK(find_project(dir.path("games/a/data/x.dta")) ==
          Symbol(dir.path("games/a/baker-project.dta")));
    CHECK(find_project(dir.path("games/a")) == Symbol(dir.path("games/a/baker-project.dta")));
    CHECK(find_project(dir.path("games/b/data")) == Symbol(dir.path("baker-project.dta")));

    // Through a reader, for content that is not on the disk.
    const LoadSession::Reader reader = [](std::string_view path, TextSink) {
        return path == "ws/game/baker-project.dta";
    };
    CHECK(find_project("ws/game/data/x.dta", reader) == Symbol("ws/game/baker-project.dta"));
    CHECK(find_project("ws/other/x.dta", reader).empty());
    CHECK(find_project("/ws/other/x.dta", reader).empty());
}

TEST_CASE("open_project sets the session and pushes the layers, leaving out what it skips") {
    TempDir dir;
    const std::string file = dir.write("baker-project.dta", kProject);
    dir.write("data/shared/common.dta", "(speed 3)\n");
    dir.write("data/default.dta",
              "(pack base (level game) (version \"1.0\"))\n"
              "#include common.dta\n"
              "#ifdef PC (input mouse) #else (input pad) #endif\n");
    dir.write("data/local.dta", "(speed 9)\n");
    dir.write("data/experiment.dta", "(speed 7)\n");
    const Project p = ok(read_project(file));

    Config cfg({.tweaks = nullptr});
    REQUIRE(open_project(cfg, p));
    CHECK(cfg.get<int>("speed") == 9);  // local's; the experiment is disabled
    CHECK(cfg.get<Symbol>("input") == Symbol("mouse"));
    CHECK(cfg.layers().size() == 4);  // three and scratch
    CHECK_FALSE(cfg.layer_enabled("experiment"));
    CHECK(cfg.read_session().pack_of(Symbol(dir.path("data/default.dta"))));

    Config shared({.tweaks = nullptr});
    REQUIRE(open_project(shared, p, {.skip = {"local"}}));
    CHECK(shared.get<int>("speed") == 3);  // from the include directory
    CHECK(shared.layers().size() == 3);
}

TEST_CASE("a project names the tool binary, its scenarios and objectives") {
    TempDir dir;
    const std::string file = dir.write("baker-project.dta",
                                       "(project\n"
                                       "   (tool \"build/bin/game\")\n"
                                       "   (scenarios \"sim\" \"extra/boss.dta\")\n"
                                       "   (objectives \"metrics\")\n"
                                       "   (layers (default \"default.dta\")))\n");
    const Project p = ok(read_project(file));  // none of them need exist
    CHECK(p.tool == Symbol(dir.path("build/bin/game")));
    CHECK(p.tool_at.line == 2);
    REQUIRE(p.scenarios.size() == 2);
    CHECK(p.scenarios[0].value == Symbol(dir.path("sim")));
    CHECK(p.scenarios[1].value == Symbol(dir.path("extra/boss.dta")));
    CHECK(p.scenarios[1].at.line == 3);
    REQUIRE(p.objectives.size() == 1);
    CHECK(p.objectives[0].value == Symbol(dir.path("metrics")));
    // A game opens it as before: the entries are the tools' business.
    dir.write("default.dta", "(speed 1)\n");
    Config cfg({.tweaks = nullptr});
    REQUIRE(open_project(cfg, p));
    CHECK(cfg.get<int>("speed") == 1);
    CHECK(p.console_port == 0);  // none named
    const std::string bad = dir.write("bad.dta", "(project (tool) (layers (d \"d.dta\")))");
    CHECK(
        std::string(read_project(bad).error().message()).find("(tool \"path\" \"argument\"...)") !=
        std::string::npos);
}

TEST_CASE("a project's tool may take arguments of its own") {
    TempDir dir;
    const std::string file =
        dir.write("baker-project.dta",
                  "(project\n   (tool \"bin/engine\" \"--path\" \".\" \"--headless\" \"--\")\n"
                  "   (layers (d \"d.dta\")))\n");
    const Project p = ok(read_project(file));
    CHECK(p.tool == Symbol(dir.path("bin/engine")));  // the binary is joined, as before
    REQUIRE(p.tool_args.size() == 4);                 // the arguments are as written
    CHECK(p.tool_args[0] == Symbol("--path"));
    CHECK(p.tool_args[1] == Symbol("."));
    CHECK(p.tool_args[3] == Symbol("--"));
    const std::string bad =
        dir.write("bad.dta", "(project (tool \"bin/engine\" 3) (layers (d \"d.dta\")))");
    CHECK(std::string(read_project(bad).error().message()).find("arguments are strings") !=
          std::string::npos);
}

TEST_CASE("a project names its game's step, for sim's results") {
    TempDir dir;
    const Project p = ok(read_project(
        dir.write("baker-project.dta", "(project (step_name turn) (layers (d \"d.dta\")))")));
    CHECK(p.step_name == Symbol("turn"));
    const auto bad = read_project(
        dir.write("bad.dta", "(project (step_name \"turn\" x) (layers (d \"d.dta\")))"));
    REQUIRE_FALSE(bad);
    CHECK(std::string(bad.error().message()).find("(step_name NAME)") != std::string::npos);
}

TEST_CASE("a project names its console's port, for tools") {
    TempDir dir;
    const std::string file =
        dir.write("baker-project.dta", "(project\n   (console 4556)\n   (layers (d \"d.dta\")))\n");
    const Project p = ok(read_project(file));
    CHECK(p.console_port == 4556);
    CHECK(p.console_at.line == 2);
    for (const char* bad : {"(console)", "(console 0)", "(console 70000)", "(console \"4556\")"}) {
        CAPTURE(bad);
        const std::string f =
            dir.write("bad.dta", "(project " + std::string(bad) + " (layers (d \"d.dta\")))");
        const auto r = read_project(f);
        REQUIRE_FALSE(r);
        CHECK(std::string(r.error().message()).find("(console PORT)") != std::string::npos);
    }
}

TEST_CASE("a layer marked dev_only never reaches a shipping build") {
    TempDir dir;
    const std::string file = dir.write("baker-project.dta",
                                       "(project (layers\n"
                                       "   (default \"default.dta\")\n"
                                       "   (local \"local.dta\" optional dev_only)\n"
                                       "   (agent \"agent.dta\" dev_only optional)))\n");
    dir.write("default.dta", "(speed 3)\n");
    dir.write("local.dta", "(speed 9)\n");
    dir.write("agent.dta", "(lives 5)\n");  // a proposal left on the build machine
    const Project p = ok(read_project(file));
    REQUIRE(p.layers.size() == 3);
    CHECK_FALSE(p.layers[0].dev_only);
    CHECK(p.layers[1].dev_only);
    CHECK(p.layers[2].dev_only);
    CHECK(p.layers[2].optional);  // either order after the path

    // What a packaging step leaves out, from the project alone.
    std::vector<std::string> excluded;
    for (const ProjectLayer& l : p.layers)
        if (l.dev_only) excluded.emplace_back(l.path.str());
    CHECK(excluded == std::vector<std::string>{dir.path("local.dta"), dir.path("agent.dta")});

    Config cfg({.tweaks = nullptr});
    REQUIRE(open_project(cfg, p));
    if constexpr (kShipping) {
        CHECK(cfg.layers().size() == 2);  // default and scratch: present files, not pushed
        CHECK(cfg.get<int>("speed") == 3);
        CHECK_FALSE(cfg.get<int>("lives"));
    } else {
        CHECK(cfg.layers().size() == 4);
        CHECK(cfg.get<int>("speed") == 9);
        CHECK(cfg.get<int>("lives") == 5);
    }
    // A check or a simulation leaves them all out with one option.
    Config tool({.tweaks = nullptr});
    REQUIRE(open_project(tool, p, {.skip_dev_only = true}));
    CHECK(tool.layers().size() == 2);
    CHECK(tool.get<int>("speed") == 3);
}

namespace {

// Options a host builds in a helper, from its own settings.
ProjectOptions options_for(bool skip_local) {
    ProjectOptions o;
    if (skip_local) o.skip.add("local");
    return o;
}

}  // namespace

TEST_CASE("project options built before the call, and kept, stay valid") {
    TempDir dir;
    const std::string file = dir.write("baker-project.dta", kProject);
    dir.write("data/shared/common.dta", "(speed 3)\n");
    dir.write("data/default.dta", "#include common.dta\n");
    dir.write("data/local.dta", "(speed 9)\n");
    const Project p = ok(read_project(file));

    // Chosen by a condition in one statement, passed in the next: the names live in the
    // options, not in a list that ended with the first statement.
    const bool skip_local = true;
    const ProjectOptions chosen = skip_local ? ProjectOptions{.skip = {"local"}} : ProjectOptions{};
    Config a({.tweaks = nullptr});
    REQUIRE(open_project(a, p, chosen));
    CHECK(a.get<int>("speed") == 3);

    Config b({.tweaks = nullptr});
    REQUIRE(open_project(b, p, options_for(true)));
    CHECK(b.get<int>("speed") == 3);
    Config c({.tweaks = nullptr});
    REQUIRE(open_project(c, p, options_for(false)));
    CHECK(c.get<int>("speed") == 9);

    // More names than the options hold: refused, rather than opening a layer it was told to
    // leave out.
    ProjectOptions many;
    const char* const names[] = {"a", "b", "c", "d", "e", "f", "g", "h", "local"};
    for (const char* n : names) many.skip.add(n);
    CHECK(many.skip.overflowed());
    Config d({.tweaks = nullptr});
    auto refused = open_project(d, p, many);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code() == ErrorCode::Refused);
    CHECK(d.layers().size() == 1);  // nothing pushed: scratch alone
}

TEST_CASE("open_project goes on past layers that do not read when asked") {
    TempDir dir;
    const std::string file = dir.write("baker-project.dta",
                                       "(project\n"
                                       "   (layers\n"
                                       "      (default \"default.dta\")\n"
                                       "      (broken \"broken.dta\")\n"
                                       "      (missing \"missing.dta\")\n"
                                       "      (top \"top.dta\")))\n");
    dir.write("default.dta", "#define SPEED (4)\n(speed 1)\n");
    dir.write("broken.dta", "(speed 2\n");
    dir.write("top.dta", "(top SPEED)\n");
    const Project p = ok(read_project(file));

    Config game({.tweaks = nullptr});
    auto first = open_project(game, p);
    REQUIRE_FALSE(first);
    CHECK(std::string(first.error().file().str()).find("broken.dta") != std::string::npos);

    Config tool({.tweaks = nullptr});
    const List<ProjectProblem> problems = ok(open_project(tool, p, {.go_on = true}));
    REQUIRE(problems.size() == 2);
    CHECK(problems[0].layer == Symbol("broken"));
    CHECK(problems[0].error.line() == 1);
    CHECK(problems[1].layer == Symbol("missing"));
    CHECK(tool.layers().size() == 3);  // default, top, and scratch
    CHECK(tool.get<int>("speed") == 1);
    CHECK(tool.get<int>("top") == 4);  // read with the macros below it

    Config fine({.tweaks = nullptr});
    TempDir clean;
    clean.write("d.dta", "(speed 5)");
    CHECK(ok(open_project(fine,
                          ok(read_project(clean.write("baker-project.dta",
                                                      "(project (layers (d \"d.dta\")))"))),
                          {.go_on = true}))
              .empty());
}

TEST_CASE("read_project reads an unsaved buffer through a reader") {
    const char* const unsaved = "(project (layers (default \"default.dta\")))";
    const LoadSession::Reader reader = [&](std::string_view path, TextSink out) {
        if (path != "proj/baker-project.dta") return false;
        out(unsaved);
        return true;
    };
    const Project p = ok(read_project("proj/baker-project.dta", reader));
    REQUIRE(p.layers.size() == 1);
    CHECK(p.layers[0].path == Symbol("proj/default.dta"));
}

TEST_CASE("a project file's mistakes are located") {
    TempDir dir;
    struct Case {
        const char* text;
        int line;
        const char* says;
    };
    const Case cases[] = {
        {"(project\n (layers (default \"d.dta\"))\n (levels a b))", 3, "has no 'levels'"},
        {"(project\n (layers\n  (default)))", 3, "a layer is"},
        {"(project\n (layers\n  (default \"d.dta\" optinal)))", 3,
         "only optional, disabled and dev_only"},
        {"(project\n (layers\n  (a \"x.dta\")\n  (a \"y.dta\")))", 4, "two layers named 'a'"},
        {"(project\n (pack_levels game))", 1, "needs (layers ...)"},
        {"(project\n (pack_levels (game)) (layers))", 2, "lists names or paths"},
        {"(layers (default \"d.dta\"))", 1, "holds (project"},
        {"(project\n #include other.dta\n (layers))", 2, "may include no files"},
        {"(project\n (layers)\n (reference a b))", 3, "(reference \"path\")"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.text);
        auto p = read_project(dir.write("baker-project.dta", c.text));
        REQUIRE_FALSE(p);
        CHECK(p.error().line() == c.line);
        CHECK(p.error().column() > 0);
        CHECK(std::string(p.error().message()).find(c.says) != std::string::npos);
    }
    CHECK_FALSE(read_project(dir.path("missing.dta")));
}

TEST_CASE("a project names a layer under a virtual root, kept as written") {
    // A host's reader serves an engine's roots; the project file is not at the root.
    std::map<std::string, std::string> files = {
        {"game/baker-project.dta",
         "(project (layers (default \"data/default.dta\")\n"
         "                 (local \"user://baker/local.dta\" optional)))"},
        {"game/data/default.dta", "(speed 1) (jump 2)"},
        {"user://baker/local.dta", "#include tuned.dta\n(speed 5)"},
        {"user://baker/tuned.dta", "(jump 7)"},
    };
    std::vector<std::string> reads;
    const LoadSession::Reader reader = [&](std::string_view path, TextSink out) {
        reads.emplace_back(path);
        const auto it = files.find(std::string(path));
        if (it == files.end()) return false;
        out(it->second);
        return true;
    };
    const Project p = ok(read_project("game/baker-project.dta", reader));
    CHECK(p.layers[1].path == Symbol("user://baker/local.dta"));  // not joined, not folded

    Config cfg({.tweaks = nullptr});
    cfg.session().set_reader(reader);
    REQUIRE(open_project(cfg, p));
    CHECK(cfg.get<int>("speed") == 5);
    CHECK(cfg.get<int>("jump") == 7);  // an include beside it, under the same root
    CHECK(std::find(reads.begin(), reads.end(), "user://baker/tuned.dta") != reads.end());
    const auto speed = cfg.source("speed");
    REQUIRE(speed);
    CHECK(speed->file == Symbol("user://baker/local.dta"));

    // A host that cannot open the root reads an optional layer as empty.
    files.erase("user://baker/local.dta");
    Config without({.tweaks = nullptr});
    without.session().set_reader(reader);
    REQUIRE(open_project(without, p));
    CHECK(without.get<int>("speed") == 1);
}

TEST_CASE("a tool path in the build directory resolves where the tool says") {
    TempDir dir;
    const std::string named = dir.write("named/baker-project.dta",
                                        "(project (build \"../out\") (tool \"$build/bin/game\")\n"
                                        "   (layers (d \"d.dta\")))\n");
    const Project p = ok(read_project(named));
    CHECK(p.tool == Symbol("$build/bin/game"));  // as written, for the tool to resolve
    CHECK(p.build == Symbol(dir.path("named/../out")));
    CHECK(p.build_at.line == 1);
    CHECK(p.tool_path() == Symbol(dir.path("named/../out/bin/game")));
    CHECK(p.tool_path("/ci/build") == Symbol("/ci/build/bin/game"));  // the tool's own wins

    // No (build ...): "build" beside the project file.
    const std::string plain = dir.write(
        "plain/baker-project.dta", "(project (tool \"$build/game\") (layers (d \"d.dta\")))\n");
    CHECK(ok(read_project(plain)).tool_path() == Symbol(dir.path("plain/build/game")));

    // A path without $build is the joined path, as before, whatever the build directory.
    const std::string fixed = dir.write("fixed/baker-project.dta",
                                        "(project (tool \"bin/game\") (layers (d \"d.dta\")))\n");
    CHECK(ok(read_project(fixed)).tool_path("/elsewhere") == Symbol(dir.path("fixed/bin/game")));

    const std::string bad =
        dir.write("bad/baker-project.dta", "(project (build) (layers (d \"d.dta\")))\n");
    CHECK(std::string(read_project(bad).error().message()).find("(build \"path\")") !=
          std::string::npos);
}
