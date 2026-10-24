// baker::check and the generated reference (M4).
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/object.hpp>
#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/std_interop.hpp>
#include <baker/std_script.hpp>
#include <baker/tools/console.hpp>
#include <baker/tools/described.hpp>
#include <baker/tools/reference.hpp>
#include <baker/tools/std_tools.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "ok.hpp"
#include "temp_dir.hpp"

using namespace baker;
using namespace baker::tools;
using baker::test::ok;

namespace {

Ref<Array> parse(std::string_view text, std::string_view name) {
    auto r = read_string(text, name);
    REQUIRE(r);
    return *r;
}

}  // namespace

TEST_CASE("check catches a wrong-arity call at load, without evaluating anything") {
    Context ctx;
    int runs = 0;
    REQUIRE(ctx.func("spawn", [&](Symbol, int64_t) { return ++runs; }));
    const auto tree = parse(
        "(level\n"
        "  (on_enter {spawn grunt 3})\n"
        "  (on_exit {spawn grunt})\n"
        "  (on_win {spawn grunt three}))\n",
        "level.dta");
    const CheckReport report = check(*tree, ctx);
    CHECK(runs == 0);
    REQUIRE(report.errors.size() == 2);
    CHECK(to_string(report.errors[0]) ==
          "baker: 'spawn' takes 2 arguments, given 1  (level.dta:3)");
    CHECK(to_string(report.errors[1]) ==
          "baker: 'spawn' is given (symbol symbol); it takes (symbol int)  (level.dta:4)");
    CHECK_FALSE(report.ok());
}

TEST_CASE("check_validator checks script at every merge, beside other validators") {
    Context ctx;
    REQUIRE(ctx.func("spawn", [](Symbol, int64_t n) { return n; }));
    Config cfg({.tweaks = nullptr});
    std::vector<std::string> seen;
    cfg.set_warning_handler([&](const Error& e) { seen.emplace_back(to_string(e)); });
    // Validators run in the order they were added.
    const auto first =
        cfg.add_validator([&](const ValidatorStack&, const Callback<void(const Error&)>&) {
            seen.emplace_back("first");
        });
    cfg.add_validator(check_validator(ctx));
    REQUIRE(cfg.push_layer("base", parse("(level (on_enter {spawn grunt 3}))", "base.dta")));
    CHECK(seen == std::vector<std::string>{"first"});

    // An overlay with a wrong call: reported at the push, located, and nothing runs.
    seen.clear();
    REQUIRE(cfg.push_layer("event", parse("(level\n  (on_exit {spawn grunt}))", "event.dta")));
    CHECK(seen == std::vector<std::string>{
                      "first", "baker: 'spawn' takes 2 arguments, given 1  (event.dta:2)"});

    // Removing a validator stops it; the check runs at every merge.
    cfg.remove_validator(first);
    seen.clear();
    REQUIRE(cfg.push_layer("fix", parse("(level (on_exit {spawn grunt 1}))", "fix.dta")));
    CHECK(seen.empty());
    if constexpr (!kShipping) {
        REQUIRE(cfg.set_layer_enabled("fix", false));
        CHECK(seen ==
              std::vector<std::string>{"baker: 'spawn' takes 2 arguments, given 1  (event.dta:2)"});
    }
}

TEST_CASE("check skips what it cannot know before running") {
    Context ctx;
    REQUIRE(ctx.func("spawn", [](Symbol, int64_t n) { return n; }));
    REQUIRE(ctx.func("log", [](Args a) { return int64_t(a.size()); }));
    const auto tree = parse(
        "{spawn grunt {+ 1 2}}\n"  // a command argument: type unknown until run
        "{spawn grunt $count}\n"   // a variable
        "{log 1 two \"three\"}\n"  // a rest parameter
        "{if 1 anything}\n"        // an unevaluated parameter
        "{+ 1 2.5}\n"              // Int promotes to Float
        "{set $x 1}\n",
        "ok.dta");
    const CheckReport report = check(*tree, ctx);
    CHECK(report.errors.empty());
    CHECK(report.demand.empty());
}

TEST_CASE("check: literal types against overloads, and {set} needs a variable") {
    Context ctx;
    const auto tree = parse("{+ 1 two}\n{set x 1}\n{not}\n", "bad.dta");
    const CheckReport report = check(*tree, ctx);
    REQUIRE(report.errors.size() == 3);
    CHECK(report.errors[0].message() ==
          "'+' is given (int symbol); it takes (int int) (float float)");
    CHECK(report.errors[1].message() == "'set' is given (symbol int); it takes ($variable value)");
    CHECK(report.errors[2].message() == "'not' takes 1 argument, given 0");
    CHECK(report.errors[2].line() == 3);
}

TEST_CASE("the demand report lists unresolved heads and stubs with every call site") {
    Context ctx;
    const auto a = parse(
        "{func boss.taunt ($who) (status stub) 0}\n"
        "{func helper ($x) $x}\n"
        "(phase1 {boss.find_cover 1} {boss.taunt player})\n"
        "(phase2\n  {boss.find_cover 2}\n  {helper 1}\n  {helper 1 2})\n",
        "boss.dta");
    const auto b = parse("{boss.find_cover 3}\n", "arena.dta");
    CheckReport report;
    check(*a, ctx, report);
    check(*b, ctx, report);
    REQUIRE(report.errors.size() == 1);
    CHECK(to_string(report.errors[0]) == "baker: 'helper' takes 1 argument, given 2  (boss.dta:7)");
    REQUIRE(report.demand.size() == 2);
    CHECK(report.demand[0].name == Symbol("boss.find_cover"));
    CHECK_FALSE(report.demand[0].stub);
    REQUIRE(report.demand[0].sites.size() == 3);
    CHECK(report.demand[0].sites[0].line == 3);
    CHECK(report.demand[0].sites[1].line == 5);
    CHECK(report.demand[0].sites[2].file == Symbol("arena.dta"));
    CHECK(report.demand[1].name == Symbol("boss.taunt"));
    CHECK(report.demand[1].stub);
    CHECK(
        to_string(report) ==
        "baker: 'helper' takes 1 argument, given 2  (boss.dta:7)\n"
        "demand: 'boss.find_cover' is not defined, called at boss.dta:3, boss.dta:5, arena.dta:1\n"
        "demand: 'boss.taunt' is a stub, called at boss.dta:3\n");
}

TEST_CASE("check over the corpus's scripts, with their functions registered by arity") {
    Context ctx;
    auto tree = read_file(BAKER_CORPUS_DIR "/scripts.dta");
    REQUIRE(tree);
    CheckReport report = check(**tree, ctx);
    // Everything the fixture calls is reported as demand; nothing is an error yet.
    CHECK(report.errors.empty());
    // Stub each demanded head with a variadic native, as a harness growing the vocabulary
    // would; then the report is clean.
    for (const auto& d : report.demand)
        REQUIRE(ctx.func(d.name, [](Args a) { return int64_t(a.size()); }));
    report = check(**tree, ctx);
    CHECK(report.errors.empty());
    CHECK(report.demand.empty());
    MESSAGE(to_string(report));
}

TEST_CASE("docs/script-reference.md matches the code") {
    Context ctx;
    const std::string generated = reference(ctx);
    const std::string path = BAKER_SOURCE_DIR "/docs/script-reference.md";
    if (std::getenv("BAKER_UPDATE_REFERENCE")) REQUIRE(write_reference(ctx, path));
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    std::string on_disk = s.str();
    on_disk.erase(std::remove(on_disk.begin(), on_disk.end(), '\r'), on_disk.end());
    CHECK_MESSAGE(on_disk == generated,
                  "docs/script-reference.md is out of date: run the script tests with "
                  "BAKER_UPDATE_REFERENCE=1 to regenerate it");
}

TEST_CASE("check applies the one-way rule to calls across packs") {
    // An in-memory file system: three packs and a loose file.
    std::map<std::string, std::string> files{
        {"task.dta",
         "(pack boss_task (level task))\n{func boss.restart_phase () 1}\n"
         "{func boss.use_engine () {engine.shake}}"},
        {"engine.dta", "(pack engine_core (level engine))\n{func engine.shake () 0}"},
        {"game.dta",
         "(pack game_rules (level game))\n#require engine.dta\n#require task.dta\n"
         "{func game.tick () {boss.restart_phase}}\n{boss.restart_phase}"},
        {"loose.dta", "#require task.dta\n{boss.restart_phase}"},
    };
    LoadSession session;
    session.set_reader([&](std::string_view path, TextSink out) {
        auto it = files.find(std::string(path));
        if (it == files.end()) return false;
        out(it->second);
        return true;
    });
    session.set_pack_levels({"engine", "game", "task"});
    Context ctx;
    auto game = test::ok(read_file("game.dta", session));
    const CheckReport report = check(*game, ctx, {.packs = &session});
    // game_rules calls boss.restart_phase twice (lines 4 and 5): warned once, at the first.
    // boss_task calling engine.shake is softer calling harder: fine.
    REQUIRE(report.warnings.size() == 1);
    CHECK(report.warnings[0].file() == Symbol("game.dta"));
    CHECK(report.warnings[0].line() == 4);
    CHECK(to_string(report.warnings[0])
              .find("pack 'game_rules' (level game) calls 'boss.restart_phase' from the softer "
                    "pack 'boss_task' (level task)") != std::string::npos);
    CHECK(report.errors.empty());
    CHECK(to_string(report).find("warning: baker: pack 'game_rules'") != std::string::npos);

    // Without the session, or from a file that declares no pack, nothing is warned.
    CHECK(check(*game, ctx).warnings.empty());
    auto loose = test::ok(read_file("loose.dta", session));
    CHECK(check(*loose, ctx, {.packs = &session}).warnings.empty());

    // A script function the Context has registered is attributed by its file too.
    auto task = test::ok(read_file("task.dta", session));
    for (const Node& n : *task) (void)test::ok(ctx.eval(n));
    files["call.dta"] = "(pack more_rules (level game))\n{boss.restart_phase}";
    auto call = test::ok(read_file("call.dta", session));
    const CheckReport registered = check(*call, ctx, {.packs = &session});
    REQUIRE(registered.warnings.size() == 1);
    CHECK(registered.warnings[0].file() == Symbol("call.dta"));
}

TEST_CASE("check reports a pack with one owner, and repetition across its trees") {
    Context ctx;
    LoadSession session;
    session.set_pack_levels({"game", "task"});
    const auto solo =
        ok(read_string("(pack boss (level task) (owners \"td-a\"))\n(x 1)", "boss.dta", session));
    const auto pair = ok(
        read_string("(pack econ (level game) (owners \"a\" \"b\"))\n(y 1)", "econ.dta", session));
    const auto none = ok(read_string("(pack ui (level game))\n(z 1)", "ui.dta", session));
    CheckReport report;
    check(*solo, ctx, report, {.packs = &session});
    REQUIRE(report.findings.size() == 1);
    CHECK(to_string(report.findings[0]) ==
          "baker: pack 'boss' has one owner, td-a: no one else keeps it  (boss.dta)");
    CHECK(report.ok());                              // a finding is never an error
    check(*pair, ctx, report, {.packs = &session});  // accumulates, without repeating it
    CHECK(report.findings.size() == 1);
    CHECK(check(*none, ctx).findings.empty());  // without the session, no findings

    // Repetition across the trees of one call, and in the text form.
    const auto a = ok(read_string("(e (hp 30) (speed 2) (loot coin))", "a.dta"));
    const auto b = ok(read_string("(e (hp 30) (speed 2) (loot coin))", "b.dta"));
    const auto c = ok(read_string("(e (hp 30) (speed 2) (loot coin))", "c.dta"));
    const Array* trees[] = {a.get(), b.get(), c.get()};
    CheckReport r;
    check(trees, ctx, r);
    REQUIRE(r.repeats.size() == 1);
    CHECK(to_string(r) ==
          "repeat: 3 times, 10 nodes: (e (hp 30) (speed 2) (loot coin)) at a.dta:1, b.dta:1, "
          "c.dta:1\n");
    CheckReport off;
    check(trees, ctx, off, {.repeats = {.min_repeats = 0}});
    CHECK(off.repeats.empty());
}

TEST_CASE("check's JSON is versioned, sorted, and the same bytes on every run") {
    // Every section has something: an error, a one-way warning, demand from two files, a
    // repeat whose form needs escaping, a one-owner finding, and packs with metadata.
    std::map<std::string, std::string> files{
        {"task.dta",
         "(pack boss_task (level task) (owners \"td-encounter\") (for designers) (stage custom))\n"
         "{func boss.restart () 1}\n"
         "(phase (say \"Get \\qready\\q\") (hp 30) (wait 2))\n"},
        {"game.dta",
         "(pack game_rules (level game) (requires boss_task) (owners \"a\" \"b\"))\n"
         "#require task.dta\n"
         "{boss.restart}\n{spawn grunt}\n{+ 1}\n"
         "(phase (say \"Get \\qready\\q\") (hp 30) (wait 2))\n"},
        {"level.dta", "{spawn grunt}\n(phase (say \"Get \\qready\\q\") (hp 30) (wait 2))\n"},
    };
    auto run = [&] {
        LoadSession session;
        session.set_reader([&](std::string_view path, TextSink out) {
            auto it = files.find(std::string(path));
            if (it == files.end()) return false;
            out(it->second);
            return true;
        });
        session.set_pack_levels({"game", "task"});
        Context ctx;
        auto game = test::ok(read_file("game.dta", session));
        auto level = test::ok(read_file("level.dta", session));
        const Array* trees[] = {game.get(), level.get()};
        CheckReport report;
        check(trees, ctx, report, {.packs = &session, .repeats = {.min_nodes = 6}});
        return to_json(report, &session);
    };
    const std::string first = run();
    CHECK(run() == first);

    const std::string path = BAKER_SOURCE_DIR "/tests/script/check_expected.json";
    if (std::getenv("BAKER_UPDATE_REFERENCE")) {
        std::ofstream(path, std::ios::binary) << first;
    }
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    std::string expected = s.str();
    expected.erase(std::remove(expected.begin(), expected.end(), '\r'), expected.end());
    CHECK_MESSAGE(expected == first,
                  "tests/script/check_expected.json differs: run the script tests with "
                  "BAKER_UPDATE_REFERENCE=1 to regenerate it, and review the diff");

    // Without the session, the packs list is empty; an empty report is still a whole one.
    CHECK(to_json(CheckReport{}) ==
          "{\n  \"schema\": \"baker.check/1.4\",\n  \"passed\": true,\n  \"errors\": [],\n"
          "  \"warnings\": [],\n  \"demand\": [],\n  \"repeats\": [],\n  \"findings\": [],\n"
          "  \"load\": [],\n  \"shape_errors\": [],\n  \"shape_warnings\": [],\n"
          "  \"tweak_errors\": [],\n  \"tweak_warnings\": [],\n"
          "  \"packs\": [],\n  \"defines\": []\n}\n");
}

TEST_CASE("the reference lists the session's packs and origins: the harness register") {
    LoadSession session;
    session.set_pack_levels({"game"});
    (void)test::ok(
        read_string("(pack econ (level game) (owners \"td-econ\") (for designers) (stage product))",
                    "econ.dta", session));
    (void)test::ok(read_string("(origin \"economy.xlsx\" (tool sheets-export))\n(price 5)",
                               "prices.dta", session));
    Context ctx;
    const std::string with = reference(ctx, &session);
    CHECK(with.find("## Packs\n\n| Pack | Level | Requires | Owners | For | Stage | File |\n"
                    "|---|---|---|---|---|---|---|\n"
                    "| `econ` | game | - | td-econ | designers | product | econ.dta |\n") !=
          std::string::npos);
    CHECK(with.find("| sheets-export | economy.xlsx | prices.dta |") != std::string::npos);
    CHECK(reference(ctx).find("## Packs") == std::string::npos);  // without the session
}

TEST_CASE("a Config's read_session has every layer's packs and origins") {
    std::map<std::string, std::string> files{
        {"default.dta",
         "(pack game_rules (level game) (owners \"a\"))\n#require task.dta\n"
         "{func game.tick () {boss.restart}}\n(x 1)"},
        {"task.dta",
         "(pack boss_task (level task) (owners \"a\" \"b\"))\n{func boss.restart () 1}"},
        {"agent.dta", "(origin \"sheet.xlsx\" (tool export))\n(x 2)"},
    };
    Config cfg({.tweaks = nullptr});
    cfg.session().set_reader([&](std::string_view path, TextSink out) {
        auto it = files.find(std::string(path));
        if (it == files.end()) return false;
        out(it->second);
        return true;
    });
    cfg.session().set_pack_levels({"engine", "game", "task"});
    Context ctx;
    std::vector<std::string> warned;
    cfg.set_warning_handler([&](const Error& e) { warned.emplace_back(to_string(e)); });
    cfg.add_validator(check_validator(ctx, {.packs = &cfg.read_session()}));
    REQUIRE(cfg.push_layer("default", "default.dta"));
    REQUIRE(cfg.push_layer("agent", "agent.dta"));

    // The base session has none of it: each layer read a copy.
    CHECK(cfg.session().packs().empty());
    const LoadSession& read = cfg.read_session();
    REQUIRE(read.packs().size() == 2);
    REQUIRE(read.origin_of(Symbol("agent.dta")));
    CHECK(read.origin_of(Symbol("agent.dta"))->source == Symbol("sheet.xlsx"));

    // The validator, given it, applies the one-way rule through the Config.
    CHECK(std::any_of(warned.begin(), warned.end(), [](const std::string& w) {
        return w.find("calls 'boss.restart' from the softer pack 'boss_task'") != std::string::npos;
    }));
    // And check finds the single-owner pack from it.
    const CheckReport report = check(*cfg.root(), ctx, {.packs = &read});
    REQUIRE(report.findings.size() == 1);
    CHECK(to_string(report.findings[0]).find("'game_rules' has one owner") != std::string::npos);

    // Removing the layer that declared the origin drops it at the next merge.
    REQUIRE(cfg.remove_layer("agent"));
    CHECK_FALSE(cfg.read_session().origin_of(Symbol("agent.dta")));
    CHECK(cfg.read_session().packs().size() == 2);
}

// --- The gate: a Config's #autorun script, host-filled problems,
// and a rule for unresolved calls. ---

TEST_CASE("checking a Config sees the script its #autorun commands define") {
    baker::test::TempDir dir;
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer(
        "default",
        dir.write("default.dta",
                  "#autorun {func helper () {nosuch 1}}\n(level (on_enter {helper}))\n")));
    Context ctx;
    REQUIRE(ctx.run_autorun(cfg.autorun().items()).empty());  // defines helper, as a host does
    // The merged tree alone does not hold the #autorun commands.
    CHECK(check(*cfg.root(), ctx).demand.empty());
    CheckReport report;
    check(cfg, ctx, report);
    REQUIRE(report.demand.size() == 1);
    CHECK(report.demand[0].name == Symbol("nosuch"));
    REQUIRE(report.demand[0].sites.size() == 1);
    CHECK(report.demand[0].sites[0].line == 1);
    CHECK(report.ok());  // unresolved calls fail only when asked

    CheckReport strict;
    check(cfg, ctx, strict, {.unresolved_fails = true});
    CHECK_FALSE(strict.ok());
    CHECK(to_json(strict).find("\"passed\": false") != std::string::npos);
}

TEST_CASE("problems the host found fail the check and are in its JSON") {
    CheckReport report;
    CHECK(report.ok());
    const Error shape(ErrorCode::Range, "speed 12 is out of range 0..10", Symbol("d.dta"), 3);
    report.shape_warnings = List<Error>(std::span<const Error>(&shape, 1));
    CHECK(report.ok());  // a warning never fails
    report.shape_errors = List<Error>(std::span<const Error>(&shape, 1));
    CHECK_FALSE(report.ok());
    const std::string json = to_json(report);
    CHECK(json.find("\"passed\": false") != std::string::npos);
    CHECK(json.find("\"shape_errors\": [\n    {\"code\": \"range\", \"file\": \"d.dta\", "
                    "\"line\": 3, \"column\": 0, \"path\": \"\", \"message\": \"speed 12 is out of "
                    "range 0..10\"}") != std::string::npos);

    CheckReport loaded;
    const Error failed(ErrorCode::NotFound, "'spawnn' is not a function", Symbol("d.dta"), 1);
    loaded.load = List<Error>(std::span<const Error>(&failed, 1));
    CHECK_FALSE(loaded.ok());
    CHECK(to_string(loaded).rfind("load: ", 0) == 0);
}

namespace {

struct Speed {
    double v = 0;
};

}  // namespace

template <>
struct baker::Convert<Speed> {
    static constexpr std::string_view name = "speed";
    static bool from(const Values& v, Speed& out) { return v.read(0, out.v); }
};

TEST_CASE("a function declared from a description checks as the native it describes") {
    // The game's functions.
    auto natives = [](Context& ctx) {
        REQUIRE(
            ctx.func("spawn", [](Symbol, int64_t, std::optional<float>) { return int64_t{1}; }));
        REQUIRE(ctx.func("pick", [](int64_t n) { return n; }));
        REQUIRE(ctx.func("pick", [](Symbol s) { return s; }));
        REQUIRE(ctx.func("say", [](std::string_view, Args) {}));
        REQUIRE(ctx.func("unless", [](Raw<Node>) { return int64_t{0}; }));
        REQUIRE(ctx.func("bump", [](Raw<Var>, const Array&) { return int64_t{0}; }));
    };
    // The same, described as the reference describes them.
    auto declared = [](Context& ctx) {
        const ParamDescription spawn[] = {{"symbol"}, {"int"}, {"float32", "optional"}};
        const ParamDescription pick_int[] = {{"int"}};
        const ParamDescription pick_sym[] = {{"symbol"}};
        const ParamDescription say[] = {{"string"}, {"values...", "rest"}};
        const ParamDescription unless[] = {{"unevaluated", "raw", true}};
        const ParamDescription bump[] = {{"$variable", "raw", true}, {"array"}};
        const OverloadDescription spawn_o[] = {{spawn, "int"}};
        const OverloadDescription pick_o[] = {{pick_int, "int"}, {pick_sym, "symbol"}};
        const OverloadDescription say_o[] = {{say, "nothing"}};
        const OverloadDescription unless_o[] = {{unless, "int"}};
        const OverloadDescription bump_o[] = {{bump, "int"}};
        REQUIRE(ctx.declare("spawn", spawn_o));
        REQUIRE(ctx.declare("pick", pick_o));
        REQUIRE(ctx.declare("say", say_o));
        REQUIRE(ctx.declare("unless", unless_o));
        REQUIRE(ctx.declare("bump", bump_o));
    };
    const auto tree = parse(
        "{spawn grunt 3}\n{spawn grunt 3 0.5}\n{spawn grunt}\n{spawn 3 grunt}\n"
        "{spawn grunt 3 fast}\n{spawn grunt 3 1 2}\n"
        "{pick 1}\n{pick a}\n{pick \"s\"}\n"
        "{say \"hi\" 1 two}\n{say hi}\n{say}\n"
        "{unless anything}\n{unless}\n"
        "{bump $x (1 2)}\n{bump x (1 2)}\n{bump $x 1}\n",
        "level.dta");
    Context game, tool;
    natives(game);
    declared(tool);
    const CheckReport a = check(*tree, game);
    const CheckReport b = check(*tree, tool);
    REQUIRE(a.errors.size() == 10);  // one for each call on lines 3-6, 9, 11-12, 14, 16-17
    REQUIRE(b.errors.size() == a.errors.size());
    for (size_t i = 0; i < a.errors.size(); ++i)
        CHECK(to_string(b.errors[i]) == to_string(a.errors[i]));

    // The reference describes them alike, but for being native code or a description.
    std::string from_game, from_tool;
    write_reference_json(game, string_out(from_game));
    write_reference_json(tool, string_out(from_tool));
    for (const char* name : {"spawn", "pick", "say", "unless", "bump"}) {
        auto entry = [&](const std::string& json) {
            const size_t at = json.find(std::string("{\"name\": \"") + name + "\"");
            return json.substr(at, json.find('\n', at) - at);
        };
        CAPTURE(name);
        CHECK(entry(from_tool) == entry(from_game));
    }

    // A call fails: there is no body.
    auto r = tool.eval("{spawn grunt 3}", "run.dta");
    REQUIRE_FALSE(r);
    CHECK(std::string(r.error().message()).find("'spawn' was declared from a description") !=
          std::string::npos);
}

TEST_CASE("a declared function keeps its status, and a description it cannot express is refused") {
    Context ctx;
    const ParamDescription one[] = {{"symbol"}};
    const OverloadDescription taunt[] = {{one, "nothing"}};
    auto declared = ctx.declare("boss.taunt", taunt);
    REQUIRE(declared);
    declared->status(Status::Stub).doc("Taunts.");
    const CheckReport report = check(*parse("{boss.taunt player}", "boss.dta"), ctx);
    REQUIRE(report.demand.size() == 1);
    CHECK(report.demand[0].stub);

    // A host type is known once a native function of this Context takes it.
    const ParamDescription fast[] = {{"speed"}};
    const OverloadDescription run[] = {{fast, "nothing"}};
    auto unknown = ctx.declare("run", run);
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().message() ==
          "cannot declare 'run': this Context has no type \"speed\", and its "
          "description gives no value types of Baker's");
    CHECK_FALSE(ctx.has_func("run"));
    REQUIRE(ctx.func("walk", [](Speed) {}));
    REQUIRE(ctx.declare("run", run));
    const CheckReport typed = check(*parse("{run 3}\n{run fast}", "run.dta"), ctx);
    REQUIRE(typed.errors.size() == 1);
    CHECK(typed.errors[0].line() == 2);

    // Nothing of a description with a mistake is registered.
    const ParamDescription bad_kind[] = {{"int", "many"}};
    const ParamDescription after_rest[] = {{"values...", "rest"}, {"int"}};
    const OverloadDescription two[] = {{one, "nothing"}, {bad_kind, "nothing"}};
    const OverloadDescription rest[] = {{after_rest, "nothing"}};
    auto kind = ctx.declare("jump", two);
    REQUIRE_FALSE(kind);
    CHECK(
        kind.error().message() ==
        "cannot declare 'jump': a parameter's kind is \"many\", not value, raw, optional, or rest");
    CHECK_FALSE(ctx.has_func("jump"));
    CHECK_FALSE(ctx.declare("jump", rest));
    CHECK_FALSE(ctx.declare("jump", std::span<const OverloadDescription>()));
    CHECK_FALSE(ctx.has_func("jump"));
}

namespace {

// The functions of a baker.reference JSON, as declare takes them: a small reader for the
// one-function-per-line layout write_reference_json writes. Views point into `json`.
struct Described {
    std::string_view name;
    std::vector<std::vector<ParamDescription>> params;
    std::vector<OverloadDescription> overloads;
};

std::string_view quoted_after(std::string_view s, std::string_view key, size_t& at) {
    at = s.find(key, at);
    REQUIRE(at != std::string_view::npos);
    at += key.size();
    const size_t end = s.find('"', at);
    const std::string_view value = s.substr(at, end - at);
    at = end + 1;
    return value;
}

std::vector<Described> described_functions(std::string_view json) {
    std::vector<Described> out;
    const size_t from = json.find("\"functions\": [");
    const size_t to = json.find("\"types\": [");
    std::string_view list = json.substr(from, to - from);
    for (size_t at = 0; (at = list.find("\n    {\"name\": \"", at)) != std::string_view::npos;) {
        const size_t end = list.find('\n', at + 1);
        const std::string_view line = list.substr(at + 1, end - at - 1);
        Described d;
        size_t p = 0;
        d.name = quoted_after(line, "{\"name\": \"", p);
        std::vector<std::string_view> results;
        while ((p = line.find("{\"params\": [", p)) != std::string_view::npos) {
            const size_t params_end = line.find("], \"result\": ", p);
            auto& ps = d.params.emplace_back();
            size_t q = p;
            while ((q = line.find("{\"type\": \"", q)) < params_end) {
                ParamDescription pd;
                pd.type = quoted_after(line, "{\"type\": \"", q);
                pd.kind = quoted_after(line, "\"kind\": \"", q);
                pd.raw = line.substr(line.find("\"raw\": ", q) + 7, 4) == "true";
                ps.push_back(pd);
            }
            p = params_end;
            results.push_back(quoted_after(line, "\"result\": \"", p));
        }
        for (size_t i = 0; i < d.params.size(); ++i)
            d.overloads.push_back({d.params[i], results[i]});
        out.push_back(std::move(d));
        at = end;
    }
    return out;
}

}  // namespace

TEST_CASE("every function Baker binds can be declared from its own reference, and checks alike") {
    // Baker's built-ins and its config tools, with no other functions.
    Context game;
    Config cfg({.tweaks = nullptr});
    REQUIRE(bind_config_tools(game, cfg));
    std::string json;
    write_reference_json(game, string_out(json));
    const std::vector<Described> functions = described_functions(json);
    size_t written = 0;  // every function in the reference is read back
    for (size_t at = 0; (at = json.find("\"tier\": ", at)) != std::string::npos; ++at) ++written;
    REQUIRE(written > 20);
    REQUIRE(functions.size() == written);

    // Each declared under another name, in a Context that has the built-ins but no tools.
    Context tool;
    std::string calls, declared_calls;
    const char* const args[] = {"",       " 1",   " a",   " \"s\"", " 1.5",        " $v",
                                " (1 2)", " a 1", " 1 a", " a b c", " \"p\" 2 3 4"};
    for (const Described& f : functions) {
        CAPTURE(f.name);
        const std::string as = "d." + std::string(f.name);
        REQUIRE(tool.declare(Symbol(as), f.overloads));
        for (const char* a : args) {
            calls += "{" + std::string(f.name) + a + "}\n";
            declared_calls += "{" + as + a + "}\n";
        }
    }
    const CheckReport native = check(*parse(calls, "calls.dta"), game);
    const CheckReport described = check(*parse(declared_calls, "calls.dta"), tool);
    REQUIRE(native.errors.size() > 100);

    // The same errors, at the same lines, but for the name: except the forms check knows
    // by their own name, whose special rules a declared function does not have.
    std::map<int32_t, std::string> by_line;
    for (const Error& e : described.errors) {
        std::string m(e.message());
        const size_t d = m.find("'d.");
        if (d != std::string::npos) m.erase(d + 1, 2);
        by_line[e.line()] = m;
    }
    size_t compared = 0;
    for (const Error& e : native.errors) {
        CAPTURE(to_string(e));
        auto it = by_line.find(e.line());
        if (it == by_line.end()) {
            const std::string m(e.message());
            // A special form's own rule, which needs its name: {set x 1} wants a variable.
            CHECK(m.find("'set'") != std::string::npos);
            continue;
        }
        CHECK(it->second == std::string(e.message()));
        ++compared;
    }
    CHECK(compared > 100);
}

TEST_CASE("check of a Config holds its data to the tweaks it binds") {
    TweakRegistry reg;
    Tweak<float> height{"jump.height", 120.0f, {.range = {16, 320}}, reg};
    Tweak<int> buffer{"jump.buffer", 3, {}, reg};
    Context ctx;
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer(
        "default", ok(read_string("(jump (height 1000) (hieght 120) (buffer 3))", "d.dta"))));
    CheckReport report;
    check(cfg, ctx, report);
    CHECK(report.tweak_errors.size() == 1);
    CHECK(report.tweak_warnings.size() == 1);
    CHECK_FALSE(report.ok());
    std::string json;
    write_json(report, string_out(json));
    CHECK(json.find("\"tweak_errors\": [\n    {\"code\": \"range\", \"file\": \"d.dta\", "
                    "\"line\": 1") != std::string::npos);

    Config none({.tweaks = nullptr});  // no registry: nothing to hold the data to
    REQUIRE(none.push_layer("default", ok(read_string("(jump (height 1000))", "d.dta"))));
    CheckReport quiet;
    check(none, ctx, quiet);
    CHECK(quiet.tweak_errors.empty());
    CHECK(quiet.ok());
}

namespace {
struct Vec2 {
    float x = 0, y = 0;
};
}  // namespace

template <>
struct baker::Convert<Vec2> {
    static constexpr std::string_view name = "vec2";
    static constexpr std::string_view values[] = {"float32", "float32"};
    static bool from(const Values& v, Vec2& out) {
        return v.size() >= 2 && v.read(0, out.x) && v.read(1, out.y);
    }
};

namespace {
struct Unit : Object<Unit> {
    static void bind(Binder<Unit>& b) {
        b.name("Unit");
        b.method("move", [](Unit&, Vec2) {});
        b.method("hold", [](Unit&) {});
    }
};
struct Tank : Object<Tank, Unit> {
    static void bind(Binder<Tank>& b) {
        b.name("Tank");
        b.method("fire", [](Tank&, int64_t n) { return n; });
    }
};
}  // namespace

TEST_CASE("types, objects and game value types declared from the reference check as the game's") {
    // The game: a type with a base, a named object of it, and a function taking a vec2.
    Context game;
    game.add_type<Tank>();
    Tank tank;
    REQUIRE(game.add(tank, "tank"));
    REQUIRE(game.func("aim", [](Vec2) {}));

    // A tool, from the reference's description of the same.
    Context tool;
    static constexpr std::string_view kVec2[] = {"float32", "float32"};
    const ParamDescription move[] = {{.type = "vec2", .values = kVec2}};
    const ParamDescription fire[] = {{"int"}};
    const OverloadDescription move_o[] = {{move, "nothing"}};
    const OverloadDescription hold_o[] = {{{}, "nothing"}};
    const OverloadDescription fire_o[] = {{fire, "int"}};
    const MethodDescription unit_methods[] = {{"move", move_o}, {"hold", hold_o}};
    const MethodDescription tank_methods[] = {{"fire", fire_o}};
    const Symbol tank_bases[] = {Symbol("Unit")};
    REQUIRE(declare_type(tool, "Unit", {}, unit_methods));
    REQUIRE(declare_type(tool, "Tank", tank_bases, tank_methods));
    REQUIRE(declare_object(tool, "tank", "Tank"));
    const OverloadDescription aim_o[] = {{move, "nothing"}};
    REQUIRE(tool.declare("aim", aim_o));

    const auto tree = parse(
        "{tank move (1 2)}\n{tank move 1}\n{tank hold}\n{tank hold 1}\n{tank fire 3}\n"
        "{tank fire x}\n{tank flyy}\n{aim (0 1)}\n{aim 1}\n{aim (0 x)}\n{tank type}\n",
        "level.dta");
    const CheckReport a = check(*tree, game);
    const CheckReport b = check(*tree, tool);
    REQUIRE(a.errors.size() == 5);  // lines 2, 4, 6, 9 and 10
    REQUIRE(b.errors.size() == a.errors.size());
    for (size_t i = 0; i < a.errors.size(); ++i)
        CHECK(to_string(b.errors[i]) == to_string(a.errors[i]));
    // A method the type does not have is demand, as for the game: a gate fails on it.
    REQUIRE(a.demand.size() == 1);
    REQUIRE(b.demand.size() == 1);
    CHECK(b.demand[0].name == a.demand[0].name);
    CHECK(b.demand[0].on == a.demand[0].on);

    // The value types are in the reference, and a declared method or object never runs.
    std::string json;
    write_reference_json(game, string_out(json));
    CHECK(json.find(R"j({"type": "vec2", "kind": "value", "raw": false, )j"
                    R"j("values": ["float32", "float32"], "count": "exact", )j"
                    R"j("element": null, "length": null})j") != std::string::npos);
    CHECK_FALSE(tool.eval("{tank hold}", "run.dta"));
}

TEST_CASE("a type or object description check cannot hold is refused, naming it") {
    Context ctx;
    const Symbol unknown_base[] = {Symbol("Vehicle")};
    auto r = declare_type(ctx, "Tank", unknown_base, {});
    REQUIRE_FALSE(r);
    CHECK(std::string(r.error().message()).find("'Tank'") != std::string::npos);
    CHECK(std::string(r.error().message()).find("'Vehicle'") != std::string::npos);

    const ParamDescription bad[] = {{"vec3"}};  // a game type with no value types
    const OverloadDescription bad_o[] = {{bad, "nothing"}};
    const MethodDescription methods[] = {{"move", bad_o}};
    r = declare_type(ctx, "Tank", {}, methods);
    REQUIRE_FALSE(r);
    CHECK(std::string(r.error().message()).find("'move'") != std::string::npos);

    REQUIRE(declare_type(ctx, "Tank", {}, {}));
    CHECK_FALSE(declare_type(ctx, "Tank", {}, {}));  // already a type
    auto o = declare_object(ctx, "tank", "Plane");
    REQUIRE_FALSE(o);
    CHECK(std::string(o.error().message()).find("'Plane'") != std::string::npos);
    REQUIRE(ctx.func("boom", [] {}));
    CHECK_FALSE(declare_object(ctx, "boom", "Tank"));  // already a function
    REQUIRE(declare_object(ctx, "tank", "Tank"));
    CHECK_FALSE(declare_object(ctx, "tank", "Tank"));  // already an object
}

TEST_CASE("a function with a body and a described signature checks and runs") {
    Context ctx;
    int moves = 0;
    int64_t last = 0;
    const ParamDescription move[] = {{"int"}};
    const OverloadDescription move_o[] = {{move, "int"}};
    REQUIRE(ctx.declare("player.move", move_o, [&](Args& a) -> Expected<Node> {
        ++moves;
        last = a.raw(0).try_int().value_or(-1);  // evaluated already: the value, not {...}
        return Node(last);
    }));
    const OverloadDescription hold_o[] = {{{}, "nothing"}};
    REQUIRE(
        ctx.declare("player.hold_jump", hold_o, [](Args&) -> Expected<Node> { return Node(); }));
    const ParamDescription later[] = {{"unevaluated", "raw", true}};
    const OverloadDescription later_o[] = {{later, "value"}};
    REQUIRE(ctx.declare("later", later_o, [](Args& a) -> Expected<Node> {
        return Node(int64_t{a.raw(0).type() == Type::Command ? 1 : 0});  // as written
    }));

    // check holds calls to the description, as for a native.
    const auto tree = parse(
        "{player.move}\n{player.hold_jump 1 2 3}\n{player.move fast}\n"
        "{player.move 2}\n",
        "scenario.dta");
    const CheckReport r = check(*tree, ctx);
    REQUIRE(r.errors.size() == 3);
    CHECK(r.errors[0].line() == 1);
    CHECK(r.errors[2].line() == 3);

    // A call runs the body, with its arguments evaluated and held to their types.
    REQUIRE(ctx.eval("{set $n 4}", "t"));
    auto ran = ctx.eval("{player.move {+ $n 1}}", "t");
    REQUIRE(ran);
    CHECK(last == 5);
    CHECK(moves == 1);
    auto wrong = ctx.eval("{player.move fast}", "t");
    REQUIRE_FALSE(wrong);
    CHECK(std::string(wrong.error().message()).find("argument 1 of 'player.move' is symbol") !=
          std::string::npos);
    CHECK(moves == 1);  // the body did not run
    CHECK(ok(ctx.eval("{later {player.move 1}}", "t")).try_int() == 1);
    CHECK(moves == 1);  // a raw argument is passed as written, not run

    // The reference lists the described parameters.
    std::string json;
    write_reference_json(ctx, string_out(json));
    CHECK(json.find(R"j({"name": "player.move", "tier": "native")j") != std::string::npos);
    CHECK(json.find(R"j({"type": "int", "kind": "value", "raw": false, "values": ["int"], )j"
                    R"j("count": "exact", "element": null, "length": null})j") !=
          std::string::npos);
}

TEST_CASE("a described parameter takes as many values as its count says") {
    Context ctx;
    static constexpr std::string_view kInt[] = {"int"};
    static constexpr std::string_view kPair[] = {"float32", "float32"};
    int64_t total = 0;
    const ParamDescription list[] = {{"list", "value", false, kInt, ValueCount::Any}};
    const OverloadDescription list_o[] = {{list, "int"}};
    REQUIRE(ctx.declare("sum", list_o, [&](Args& a) -> Expected<Node> {
        total = 0;
        if (const Array* xs = a.raw(0).try_array())
            for (const Node& x : xs->nodes()) total += x.try_int().value_or(0);
        return Node(total);
    }));
    const ParamDescription pair[] = {{"vec2", "value", false, kPair}};  // exact, the default
    const OverloadDescription pair_o[] = {{pair, "nothing"}};
    REQUIRE(ctx.declare("aim", pair_o, [](Args&) -> Expected<Node> { return Node(); }));

    const auto tree =
        parse("{sum (1 2 3)}\n{sum (1 x)}\n{sum ()}\n{aim (1 2)}\n{aim (1 2 3)}\n", "scenario.dta");
    const CheckReport r = check(*tree, ctx);
    REQUIRE(r.errors.size() == 2);
    CHECK(r.errors[0].line() == 2);  // a symbol in a list of ints
    CHECK(r.errors[1].line() == 5);  // three values where a vec2 takes two

    REQUIRE(ctx.eval("{sum (4 5 6)}", "t"));
    CHECK(total == 15);
    CHECK_FALSE(ctx.eval("{sum (4 five)}", "t"));
    CHECK_FALSE(ctx.eval("{aim (1 2 3)}", "t"));

    std::string json;
    write_reference_json(ctx, string_out(json));
    CHECK(json.find(R"j("values": ["int"], "count": "any")j") != std::string::npos);
}

TEST_CASE("a tool tells a type already known from a refusal without reading words") {
    Context ctx;
    CHECK(has_type(ctx, "Scheduler"));  // Baker's own, as every Context has it
    CHECK_FALSE(has_type(ctx, "Tank"));
    const auto again = declare_type(ctx, "Scheduler", {}, {});
    REQUIRE_FALSE(again);
    CHECK(again.error().code() == ErrorCode::Refused);
    const Symbol unknown[] = {Symbol("Vehicle")};
    const auto base = declare_type(ctx, "Tank", unknown, {});
    REQUIRE_FALSE(base);
    CHECK(base.error().code() == ErrorCode::Type);
    REQUIRE(declare_type(ctx, "Tank", {}, {}));
    CHECK(has_type(ctx, "Tank"));
    CHECK(declare_object(ctx, "scheduler", "Tank").error().code() == ErrorCode::Refused);
    CHECK(declare_object(ctx, "t", "Plane").error().code() == ErrorCode::Type);
}
