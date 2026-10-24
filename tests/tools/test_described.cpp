// Tweaks a tool makes from the reference's descriptions, held to as the game's tweaks are.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>
#include <baker/tools/described.hpp>
#include <baker/tools/reference.hpp>

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "ok.hpp"

using namespace baker;
using namespace baker::tools;
using baker::test::ok;

namespace {
std::vector<std::string> messages(const List<Error>& list) {
    std::vector<std::string> out;
    for (const Error& e : list) out.emplace_back(e.message());
    return out;
}
}  // namespace

namespace {
Ref<Array> parse(std::string_view text, std::string_view name) {
    return ok(read_string(text, name));
}

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
    static void to(const Vec2& v, ValueList& out) {
        out.add(v.x);
        out.add(v.y);
    }
};

TEST_CASE("a tool registers tweaks from their descriptions, and check holds data to them") {
    // The game's own tweaks, and a tool's copy of them from the reference's descriptions.
    TweakRegistry game;
    Tweak<float> height{"jump.height", 120.0f, {.range = {16, 320}}, game};
    Tweak<Vec2> aim{"jump.aim", Vec2{0, 1}, {}, game};
    static constexpr std::string_view kSurfaces[] = {"stone", "ice"};
    Tweak<Symbol> floor{"world.floor", Symbol("stone"), {.one_of = kSurfaces}, game};

    TweakRegistry tool;
    DescribedTweaks described(tool);
    static constexpr std::string_view kVec2[] = {"float32", "float32"};
    REQUIRE(described.add(
        {.path = "jump.height", .type = "float32", .default_text = "120.0", .range = {16, 320}}));
    REQUIRE(described.add(
        {.path = "jump.aim", .type = "vec2", .values = kVec2, .default_text = "(0.0 1.0)"}));
    REQUIRE(described.add(
        {.path = "world.floor", .type = "symbol", .default_text = "stone", .one_of = kSurfaces}));
    CHECK(described.size() == 3);
    CHECK(tool.size() == 3);

    // Each finding is the game's own, for the same data.
    for (const char* text : {"(jump (height 120) (aim 0 1))\n(world (floor ice))\n",
                             "(jump (height \"tall\") (aim 0 1))\n(world (floor ice))\n",
                             "(jump (height 1000) (aim 1))\n(world (floor lava))\n",
                             "(jump (hieght 120) (aim 0.5 x))\n"}) {
        CAPTURE(text);
        const Ref<Array> root = ok(read_string(text, "d.dta"));
        const TweakFindings mine = check_tweaks(*root, tool);
        const TweakFindings theirs = check_tweaks(*root, game);
        CHECK(messages(mine.errors) == messages(theirs.errors));
        CHECK(messages(mine.warnings) == messages(theirs.warnings));
    }
    const TweakFindings wrong =
        check_tweaks(*ok(read_string("(jump (height 120) (aim 1))\n", "d.dta")), tool);
    REQUIRE(wrong.errors.size() == 1);  // a vec2 wanted
    CHECK(std::string(wrong.errors[0].message()).find("vec2") != std::string::npos);

    // The JSON of the described tweaks names their types as the game's does.
    std::string json;
    write_json(tool, string_out(json));
    CHECK(json.find(R"j("type": "vec2", "values": ["float32", "float32"], "count": "exact", )j"
                    R"j("element": null, "length": null, "default": "(0.0 1.0)")j") !=
          std::string::npos);
}

TEST_CASE("a description the registry cannot hold is an error naming the tweak") {
    TweakRegistry reg;
    DescribedTweaks described(reg);
    const auto no_values = described.add({.path = "jump.aim", .type = "vec2"});
    REQUIRE_FALSE(no_values);
    CHECK(std::string(no_values.error().message()).find("'jump.aim'") != std::string::npos);
    static constexpr std::string_view kUnknown[] = {"float64"};
    const auto unknown = described.add({.path = "a", .type = "big", .values = kUnknown});
    REQUIRE_FALSE(unknown);
    CHECK(std::string(unknown.error().message()).find("float64") != std::string::npos);
    const auto bad_default =
        described.add({.path = "b", .type = "float32", .default_text = "tall"});
    REQUIRE_FALSE(bad_default);
    CHECK(std::string(bad_default.error().message()).find("default") != std::string::npos);
    CHECK(described.size() == 0);
    CHECK(reg.size() == 0);
}

TEST_CASE("a fixed-size list declares its value types, and a tool checks it as the game") {
    TweakRegistry game;
    Tweak<std::array<float, 3>> color{"cube.color", {0.9f, 0.5f, 0.2f}, {}, game};
    TweakRegistry nested;
    Tweak<std::array<Vec2, 2>> path{"cube.path", {}, {}, nested};
    REQUIRE(color.value_types().size() == 3);
    CHECK(path.value_types().empty());  // values nested in each element: not listed flat
    std::string json;
    write_json(game, string_out(json));
    CHECK(json.find(
              R"j("type": "fixed-size list", "values": ["float32", "float32", "float32"], )j") !=
          std::string::npos);

    TweakRegistry tool;
    DescribedTweaks described(tool);
    REQUIRE(described.add({.path = "cube.color",
                           .type = "fixed-size list",
                           .values = color.value_types(),
                           .count = color.value_count(),
                           .default_text = "(0.9 0.5 0.2)"}));
    // Four values where three are wanted: the game's converter refuses them, and now the
    // tool's description does too (it took three or more before).
    for (const char* text : {"(cube (color 0.9 0.5 0.2))\n", "(cube (color 0.9 0.5))\n",
                             "(cube (color red 0.5 0.2))\n", "(cube (color 0.9 0.5 0.2 0.1))\n"}) {
        CAPTURE(text);
        const Ref<Array> root = ok(read_string(text, "d.dta"));
        CHECK(messages(check_tweaks(*root, tool).errors) ==
              messages(check_tweaks(*root, game).errors));
        CHECK(messages(check_tweaks(*root, tool).warnings) ==
              messages(check_tweaks(*root, game).warnings));
    }
}

TEST_CASE("string and integer tweaks take allowed values, checked and in the JSON") {
    TweakRegistry reg;
    static constexpr std::string_view kAttacks[] = {"light", "heavy", "kick"};
    Tweak<std::string_view> attack{"fighter.attack", "light", {.one_of = kAttacks}, reg};
    static constexpr TweakChoice kLevels[] = {{0, "easy"}, {1, "normal"}, {2, "hard"}};
    Tweak<int> level{"fighter.level", 1, {.choices = kLevels}, reg};
    Tweak<int> team{"fighter.team", 0, {}, reg};
    const auto run = [&](const char* text) {
        return messages(check_tweaks(*ok(read_string(text, "d.dta")), reg).errors);
    };

    CHECK(
        run("(fighter (attack \"heavy\") (level 2) (team 0 (one_of (0 red) (1 blue))))\n").empty());
    auto bad = run("(fighter (attack \"hevy\") (level 5) (team 3 (one_of (0 red) 1)))\n");
    REQUIRE(bad.size() == 3);
    CHECK(bad[0] == "'fighter.attack' is 'hevy', not one of its tweak's light, heavy, kick");
    CHECK(bad[1] == "'fighter.level' is 5, not one of its tweak's 0 (easy), 1 (normal), 2 (hard)");
    CHECK(bad[2] == "'fighter.team' is 3, not one of its tweak's 0 (red), 1");  // the data's
    // The data's own list wins over the code's.
    CHECK(run("(fighter (attack \"punch\" (one_of \"punch\" \"kick\")) (level 7 (one_of 7)))\n")
              .empty());
    CHECK(attack.one_of().size() == 3);
    REQUIRE(level.choices().size() == 3);
    CHECK(level.choices()[2].name == "hard");

    std::string json;
    write_json(reg, string_out(json));
    CHECK(json.find(R"j("one_of": ["light", "heavy", "kick"], "choices": [])j") !=
          std::string::npos);
    CHECK(
        json.find(R"j("choices": [{"value": 0, "name": "easy"}, {"value": 1, "name": "normal"}, )j"
                  R"j({"value": 2, "name": "hard"}])j") != std::string::npos);

    // A tool's described copy finds the same.
    TweakRegistry tool;
    DescribedTweaks described(tool);
    REQUIRE(described.add({.path = "fighter.attack",
                           .type = "string",
                           .default_text = "\"light\"",
                           .one_of = kAttacks}));
    REQUIRE(described.add(
        {.path = "fighter.level", .type = "int32", .default_text = "1", .choices = kLevels}));
    const Ref<Array> root = ok(read_string("(fighter (attack \"hevy\") (level 5))\n", "d.dta"));
    TweakRegistry only;  // the game's two, without team
    Tweak<std::string_view> attack2{"fighter.attack", "light", {.one_of = kAttacks}, only};
    Tweak<int> level2{"fighter.level", 1, {.choices = kLevels}, only};
    CHECK(messages(check_tweaks(*root, tool).errors) == messages(check_tweaks(*root, only).errors));
}

TEST_CASE("a list tweak is described and checked as the game's") {
    TweakRegistry game;
    Tweak<std::vector<int>> waves{"level.waves", {3, 4}, {}, game};
    REQUIRE(waves.value_types().size() == 1);
    CHECK(waves.value_count() == ValueCount::Any);

    TweakRegistry tool;
    DescribedTweaks described(tool);
    REQUIRE(described.add({.path = "level.waves",
                           .type = "list",
                           .values = waves.value_types(),
                           .count = waves.value_count(),
                           .element = waves.list_element(),
                           .default_text = "(3 4)"}));
    for (const char* text : {"(level (waves 3 4 5))\n", "(level (waves 7))\n", "(level (waves))\n",
                             "(level (waves 3 2.5))\n", "(level (waves 3 many))\n"}) {
        CAPTURE(text);
        const Ref<Array> root = ok(read_string(text, "d.dta"));
        CHECK(messages(check_tweaks(*root, tool).errors) ==
              messages(check_tweaks(*root, game).errors));
    }
    const Ref<Array> bad = ok(read_string("(level (waves 3 2.5))\n", "d.dta"));
    CHECK(check_tweaks(*bad, tool).errors.size() == 1);  // a non-integer inside the list

    // The JSON of the described tweak says as much as the game's.
    std::string mine, theirs;
    write_json(tool, string_out(mine));
    write_json(game, string_out(theirs));
    const char* const listed =
        R"j("values": ["int32"], "count": "any", )j"
        R"j("element": {"type": "int32", "values": ["int32"], "count": "exact"}, "length": null, )j"
        R"j("default": "(3 4)")j";
    CHECK(mine.find(listed) != std::string::npos);
    CHECK(theirs.find(listed) != std::string::npos);
}

// --- A list describes its elements (requirement 0025). ---

TEST_CASE("a list of the game's own type describes its element and length") {
    TweakRegistry game;
    Tweak<std::vector<Vec2>> patrol{"guard.patrol", {{0, 0}, {4, 0}}, {}, game};
    Tweak<std::array<Vec2, 4>> quad{"zone.quad", {}, {}, game};
    Tweak<std::vector<float>> weights{"loot.weights", {1.0f, 2.0f}, {}, game};
    Tweak<Vec2> aim{"guard.aim", Vec2{0, 1}, {}, game};
    CHECK(patrol.list_element().type == "vec2");
    CHECK(patrol.list_element().values.size() == 2);
    CHECK(patrol.list_element().length == 0);
    CHECK(quad.list_element().length == 4);
    CHECK(patrol.value_types().empty());  // its values nest, and cannot be listed flat
    CHECK(aim.list_element().type.empty());

    // The data gives the weights one element.
    Config cfg({.tweaks = &game});
    REQUIRE(cfg.push_layer("default", ok(read_string("(loot (weights 1.0))\n", "d.dta"))));
    std::string json;
    write_json(game, string_out(json));
    CHECK(json.find(R"j("path": "guard.patrol", "type": "list", "values": null, "count": null, )j"
                    R"j("element": {"type": "vec2", "values": ["float32", "float32"], )j"
                    R"j("count": "exact"}, "length": null, )j"
                    R"j("default": "((0.0 0.0) (4.0 0.0))")j") != std::string::npos);
    CHECK(json.find(R"j("path": "zone.quad", "type": "fixed-size list", "values": null, )j"
                    R"j("count": null, "element": {"type": "vec2", )j"
                    R"j("values": ["float32", "float32"], "count": "exact"}, "length": 4, )j") !=
          std::string::npos);
    // A list of one element is written in parentheses, so it reads back as a list.
    CHECK(json.find(R"j("path": "loot.weights", "type": "list", "values": ["float32"], )j"
                    R"j("count": "any", "element": {"type": "float32", "values": ["float32"], )j"
                    R"j("count": "exact"}, "length": null, "default": "(1.0 2.0)", )j"
                    R"j("value": "(1.0)")j") != std::string::npos);
    CHECK(json.find(R"j("path": "guard.aim", "type": "vec2", "values": ["float32", "float32"], )j"
                    R"j("count": "exact", "element": null, "length": null, )j") !=
          std::string::npos);
}

TEST_CASE("a tool holds a list's elements to their type as the game does") {
    TweakRegistry game;
    Tweak<std::vector<Vec2>> patrol{"guard.patrol", {{0, 0}}, {.range = {0, 10}}, game};
    Tweak<std::array<Vec2, 4>> quad{"zone.quad", {}, {}, game};
    Tweak<std::vector<int>> spawns{"wave.spawns", {3, 4}, {}, game};
    static constexpr std::string_view kTags[] = {"fast", "loud"};
    Tweak<std::vector<Symbol>> tags{"wave.tags", {}, {.one_of = kTags}, game};

    // The tool's copies, made from the descriptions alone, as the JSON gives them.
    TweakRegistry tool;
    DescribedTweaks described(tool);
    static constexpr std::string_view kVec2[] = {"float32", "float32"};
    static constexpr std::string_view kInt[] = {"int32"};
    static constexpr std::string_view kSymbol[] = {"symbol"};
    REQUIRE(described.add({.path = "guard.patrol",
                           .type = "list",
                           .count = ValueCount::Any,
                           .element = {.type = "vec2", .values = kVec2},
                           .default_text = "((0.0 0.0))",
                           .range = {0, 10}}));
    REQUIRE(described.add({.path = "zone.quad",
                           .type = "fixed-size list",
                           .element = {.type = "vec2", .values = kVec2, .length = 4},
                           .default_text = "((0.0 0.0) (0.0 0.0) (0.0 0.0) (0.0 0.0))"}));
    REQUIRE(described.add({.path = "wave.spawns",
                           .type = "list",
                           .values = kInt,
                           .count = ValueCount::Any,
                           .element = {.type = "int32"},
                           .default_text = "(3)"}));
    REQUIRE(described.add({.path = "wave.tags",
                           .type = "list",
                           .values = kSymbol,
                           .count = ValueCount::Any,
                           .element = {.type = "symbol"},
                           .default_text = "()",
                           .one_of = kTags}));

    const char* const good =
        "(guard (patrol (0 0) (4 0) (4 4)))\n(zone (quad (0 0) (1 0) (1 1) (0 1)))\n"
        "(wave (spawns 3 4 5) (tags fast loud))\n";
    for (const char* text : {
             good,
             "(guard (patrol (0 0) (4)))\n",       // a point given one number
             "(guard (patrol (0 0) (40 0)))\n",    // outside the range, inside an element
             "(zone (quad (0 0) (1 0) (1 1)))\n",  // three points of four
             "(wave (spawns 3 four 5))\n",         // an element of the wrong type
             "(wave (tags fast quiet))\n",         // outside one_of
         }) {
        CAPTURE(text);
        const Ref<Array> root = ok(read_string(text, "d.dta"));
        const TweakFindings mine = check_tweaks(*root, tool);
        const TweakFindings theirs = check_tweaks(*root, game);
        CHECK(messages(mine.errors) == messages(theirs.errors));
        CHECK(mine.errors.size() == (text == good ? 0u : 1u));
    }

    // An element whose type says nothing is refused, naming the tweak.
    const auto bare = described.add({.path = "guard.route",
                                     .type = "list",
                                     .count = ValueCount::Any,
                                     .element = {.type = "vec3"},
                                     .default_text = "()"});
    REQUIRE_FALSE(bare);
    CHECK(std::string(bare.error().message()).find("tweak 'guard.route'") != std::string::npos);
    CHECK(std::string(bare.error().message()).find("element type 'vec3'") != std::string::npos);
}

TEST_CASE("a list parameter's elements are checked, native or declared, and at a call") {
    static constexpr std::string_view kVec2[] = {"float32", "float32"};
    const ParamDescription walk[] = {
        {.type = "list", .count = ValueCount::Any, .element = {.type = "vec2", .values = kVec2}}};
    const OverloadDescription walk_o[] = {{walk, "nothing"}};
    Context game, tool;
    REQUIRE(game.func("guard.walk", [](std::vector<Vec2>) {}));
    REQUIRE(tool.declare("guard.walk", walk_o, [](Args&) -> Expected<Node> { return Node(); }));

    // The reference describes the native's parameter as the description does.
    std::string json;
    write_reference_json(game, string_out(json));
    CHECK(json.find(R"j("element": {"type": "vec2", "values": ["float32", "float32"], )j"
                    R"j("count": "exact"}, "length": null})j") != std::string::npos);

    const auto good = parse("{guard.walk ((0 0) (1 1))}", "t.dta");
    const auto bad = parse("{guard.walk ((0 0) (1))}", "t.dta");
    CHECK(check(*good, game).errors.empty());
    CHECK(check(*good, tool).errors.empty());
    CHECK(check(*bad, game).errors.size() == 1);
    CHECK(check(*bad, tool).errors.size() == 1);
    // A list with an evaluated element is left to the call.
    CHECK(check(*parse("{guard.walk ((0 0) {pick})}", "t.dta"), tool).errors.empty());

    CHECK(tool.eval("{guard.walk ((0 0) (1 1))}", "t"));
    CHECK_FALSE(tool.eval("{guard.walk ((0 0) (1))}", "t"));
}

TEST_CASE("a described curve tweak holds data to a curve, and its range to each y") {
    TweakRegistry tool;
    DescribedTweaks described(tool);
    REQUIRE(described.add(
        {.path = "falloff", .type = "curve", .default_text = "((0 1) (10 0))", .range = {0, 1}}));
    CHECK_FALSE(described.add({.path = "bad", .type = "curve", .default_text = "((5 1) (0 0))"}));
    CHECK(tool.size() == 1);  // a refused tweak leaves the others registered
    auto findings = [&](const char* text) {
        const auto tree = ok(read_string(text, "t.dta"));
        std::vector<std::string> out;
        for (const Error& e : check_tweaks(*tree, tool).errors) out.emplace_back(to_string(e));
        return out;
    };
    CHECK(findings("(falloff linear (0 1) (5 0.5) (10 0))").empty());
    CHECK(findings("(falloff (0 1) (5 2))").size() == 1);    // y outside the range
    CHECK(findings("(falloff (0 1) (0 0.5))").size() == 1);  // x not ascending
    CHECK(findings("(falloff wobbly (0 1))").size() == 1);
}
