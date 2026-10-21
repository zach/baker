// The machine-readable reference, baker.reference/1.2.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/object.hpp>
#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/std_interop.hpp>

#include <string>
#include <vector>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;

namespace {

struct Lamp : Object<Lamp> {
    float level = 1;
    static void bind(Binder<Lamp>& b) {
        b.name("Lamp");
        b.method("dim", [](Lamp& l, float by) { return l.level -= by; }).doc("Dims by `by`.");
        b.property("level", &Lamp::level, "Brightness, 0 to 1.");
    }
};

std::string reference_json(const Context& ctx, const LoadSession* packs = nullptr) {
    std::string out;
    write_reference_json(ctx, string_out(out), packs);
    return out;
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("the reference as JSON lists functions, types, classes, globals, and packs") {
    Context ctx;
    REQUIRE(ctx.func("spawn", [](Symbol, std::optional<int64_t>) { return int64_t{1}; }));
    ctx.add_type<Lamp>();
    ctx.global("difficulty", Node(int64_t{2}), "1 to 5.");
    REQUIRE(
        ctx.eval("{func twice ($a) {* $a 2}}\n"
                 "{class Lamp (flicker () 1)}\n"
                 "{class Ghost (haunt () 1)}",
                 "lib.dta"));
    const std::string json = reference_json(ctx);
    CHECK(json.rfind("{\n  \"schema\": \"baker.reference/1.2\",\n  \"functions\": [", 0) == 0);
    CHECK(json == reference_json(ctx));  // the same bytes, run to run
    CHECK(json.back() == '\n');

    // A native function with an optional parameter.
    CHECK(has(json, R"j({"name": "spawn", "tier": "native", "status": "normal", )j"
                    R"j("replacement": "", "doc": "", "file": "", "line": 0, "overloads": )j"
                    R"j([{"params": [{"type": "symbol", "kind": "value", "raw": false, )j"
                    R"j("values": ["symbol"]}, {"type": "int", "kind": "optional", )j"
                    R"j("raw": false, "values": ["int"]}], )j"
                    R"j("result": "int", "min_args": 1, "max_args": 2}]})j"));
    // A script function, with where it is defined.
    CHECK(has(json, R"j({"name": "twice", "tier": "script",)j"));
    CHECK(has(json, R"j("file": "lib.dta", "line": 1,)j"));
    // A built-in that takes the rest of its arguments.
    CHECK(has(json, R"j("max_args": null)j"));
    // A type: its method, property, and the handler script attached to it.
    CHECK(has(json, R"j({"name": "Lamp", "bases": [], "methods": [{"name": "dim", )j"
                    R"j("status": "normal", "replacement": "", "doc": "Dims by `by`.",)j"));
    CHECK(has(json, R"j("properties": [{"name": "level", "type": "float32", )j"
                    R"j("doc": "Brightness, 0 to 1."}], "handlers": ["flicker"]})j"));
    // A class only script defines.
    CHECK(has(json, R"j("script_classes": [)j"
                    "\n"
                    R"j(    {"name": "Ghost", "handlers": ["haunt"]})j"));
    CHECK(has(json, R"j("globals": [)j"
                    "\n"
                    R"j(    {"name": "difficulty", "doc": "1 to 5."})j"));
    CHECK(has(json,
              "  \"objects\": [\n"
              R"j(    {"name": "scheduler", "type": "Scheduler"})j"  // the Context's own
              "\n  ],\n  \"packs\": [],\n  \"origins\": []\n}\n"));
}

TEST_CASE("the reference lists the objects added under a name, with their types") {
    Context ctx;
    Lamp hall, porch, unnamed;
    REQUIRE(ctx.add(porch, "porch"));
    REQUIRE(ctx.add(hall, "hall"));
    REQUIRE(ctx.add(unnamed));
    REQUIRE(ctx.eval("{class Ghost (haunt () 1)}\n{new Ghost attic}", "lib.dta"));
    std::string json = reference_json(ctx);
    CHECK(has(json,
              "  \"objects\": [\n"
              R"j(    {"name": "attic", "type": "Ghost"},)j"
              "\n"
              R"j(    {"name": "hall", "type": "Lamp"},)j"
              "\n"
              R"j(    {"name": "porch", "type": "Lamp"},)j"
              "\n"
              R"j(    {"name": "scheduler", "type": "Scheduler"})j"
              "\n  ],\n"));

    // Those present when it is written.
    ctx.remove(hall);
    json = reference_json(ctx);
    CHECK_FALSE(has(json, R"j("hall")j"));
    CHECK(has(json, R"j({"name": "porch", "type": "Lamp"})j"));
}

TEST_CASE("given a session, the reference lists its packs and origins") {
    Context ctx;
    LoadSession session;
    session.set_pack_levels({"engine", "game", "task"});
    REQUIRE(
        read_string("(pack boss (level game) (owners \"td\") (stage custom))\n"
                    "(origin \"economy.xlsx\" (tool sheets-export))\n",
                    "boss.dta", session));
    const std::string json = reference_json(ctx, &session);
    CHECK(has(json, R"j({"name": "boss", "level": "game", "file": "boss.dta", )j"
                    R"j("requires": [], "owners": ["td"], "for": [], "stage": "custom"})j"));
    CHECK(
        has(json, R"j({"tool": "sheets-export", "source": "economy.xlsx", "file": "boss.dta"})j"));
}

TEST_CASE("script_functions lists the {func}s a load's #autorun defines, without running them") {
    LoadSession session;
    REQUIRE(
        read_string("[HELPER {func helper ($x) $x}]\n"
                    "#autorun {func spawn_wave ($n) {print $n}}\n"
                    "#autorun {if 1 {func nested () 0}}\n"
                    "#autorun {if 1 HELPER}\n"
                    "#autorun {func spawn_wave () 0}\n",
                    "boss.dta", session));
    const List<Definition> fs = script_functions(session.autorun());
    std::vector<std::string> got;
    for (const Definition& d : fs)
        got.push_back(std::string(d.name.str()) + "@" + std::string(d.file.str()) + ":" +
                      std::to_string(d.line));
    CHECK(got == std::vector<std::string>{"spawn_wave@boss.dta:2", "nested@boss.dta:3",
                                          "helper@boss.dta:1", "spawn_wave@boss.dta:5"});
    Context ctx;
    CHECK_FALSE(ctx.has_func("spawn_wave"));  // nothing ran

    // A Config keeps its enabled file layers' commands, not its read session.
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("base.dta", "#autorun {func base_fn () 0}\n(a 1)"));
    REQUIRE(cfg.push_layer_text("mod.dta", "\n#autorun {func mod_fn () 0}\n(b 1)"));
    List<Definition> layered = script_functions(cfg.autorun().items());
    REQUIRE(layered.size() == 2);
    CHECK(layered[0].name == Symbol("base_fn"));
    CHECK(layered[1].name == Symbol("mod_fn"));
    CHECK(layered[1].file == Symbol("mod.dta"));
    CHECK(layered[1].line == 2);
    if constexpr (!kShipping) {
        REQUIRE(cfg.set_layer_enabled("mod.dta", false));  // a disabled layer's are left out
        CHECK(script_functions(cfg.autorun().items()).size() == 1);
    }
}

TEST_CASE("the macros a layer brought go with the layer") {
    Config cfg({.tweaks = nullptr});
    REQUIRE(cfg.push_layer_text("base.dta", "[SPEED 3]\n(speed SPEED)"));
    REQUIRE(cfg.push_layer_text("mod.dta", "#define HARD\n(hard 1)"));
    auto names = [&] {
        std::vector<std::string> out;
        for (const Definition& d : cfg.read_session().macro_definitions())
            out.emplace_back(d.name.str());
        return out;
    };
    CHECK(names() == std::vector<std::string>{"SPEED", "HARD"});
    if constexpr (!kShipping) {
        REQUIRE(cfg.remove_layer("mod.dta"));
        CHECK(names() == std::vector<std::string>{"SPEED"});
    }
}
