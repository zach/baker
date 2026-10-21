// The tweaks as JSON, baker.tweaks/1.3.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;

TEST_CASE("the tweaks as JSON: type, default, value, metadata, and source") {
    TweakRegistry reg;
    Tweak<float> speed{
        "player.speed", 4.0f, {.range = {0, 10}, .step = 0.5, .desc = "Run speed"}, reg};
    Tweak<int> lives{"player.lives", 3, {.dev_only = true}, reg};
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("default", ok(read_string("(player (speed 6))\n", "d.dta"))));
    std::string json;
    write_json(reg, string_out(json));
    CHECK(json ==
          "{\n  \"schema\": \"baker.tweaks/1.3\",\n  \"tweaks\": [\n"
          R"j(    {"path": "player.lives", "type": "int32", "values": ["int32"], "default": "3", )j"
          R"j("value": "3", )j"
          R"j("bound": false, "range": null, "step": 0.0, "desc": "", "kind": "", )j"
          R"j("group": "player", "dev_only": true, "one_of": [], "choices": [], "source": null},)j"
          "\n"
          R"j(    {"path": "player.speed", "type": "float32", "values": ["float32"], )j"
          R"j("default": "4.0", "value": "6.0", )j"
          R"j("bound": true, "range": [0.0, 10.0], "step": 0.5, "desc": "Run speed", )j"
          R"j("kind": "", "group": "player", "dev_only": false, "one_of": [], "choices": [], )j"
          R"j("source": {"layer": "default", "file": "d.dta", "line": 1}})j"
          "\n  ]\n}\n");
}

TEST_CASE("a symbol tweak's allowed symbols, from the code or the data") {
    static constexpr std::string_view kButtons[] = {"light", "heavy"};
    TweakRegistry reg;
    Tweak<Symbol> jab{"moves.jab.button", Symbol("light"), {.one_of = kButtons}, reg};
    Tweak<Symbol> kick{"moves.kick.button", Symbol("light"), {.one_of = kButtons}, reg};
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer(
        "default", ok(read_string("(moves (jab (button heavy)) (kick (button kick (one_of light "
                                  "heavy kick))))\n",
                                  "d.dta"))));
    CHECK(Symbol(jab) == Symbol("heavy"));
    CHECK(Symbol(kick) == Symbol("kick"));  // the metadata is not part of the value
    CHECK(jab.one_of().size() == 2);        // the code's
    CHECK(kick.one_of().size() == 3);       // the data's
    std::string json;
    write_json(reg, string_out(json));
    CHECK(json.find(R"j("one_of": ["light", "heavy"], "choices": [], "source")j") !=
          std::string::npos);
    CHECK(json.find(R"j("one_of": ["light", "heavy", "kick"], "choices": [], "source")j") !=
          std::string::npos);
}

namespace {
std::vector<std::string> messages(const List<Error>& list) {
    std::vector<std::string> out;
    for (const Error& e : list) out.emplace_back(e.message());
    return out;
}
}  // namespace

TEST_CASE("the data is held to the tweaks the game declares") {
    TweakRegistry reg;
    Tweak<float> height{"jump.height", 120.0f, {.range = {16, 320}}, reg};
    Tweak<int> buffer{"jump.buffer", 3, {}, reg};
    Tweak<float> air{"jump.air.time", 0.5f, {}, reg};
    static constexpr std::string_view kSurfaces[] = {"stone", "ice"};
    Tweak<Symbol> floor{"world.floor", Symbol("stone"), {.one_of = kSurfaces}, reg};
    Tweak<float> gravity{"world.gravity", 9.8f, {}, reg};
    const auto run = [&](const char* text) {
        return check_tweaks(*ok(read_string(text, "d.dta")), reg);
    };

    // Clean data: nothing, and structured data at the top level is quiet.
    const auto clean =
        run("(jump (height 120) (buffer 3) (air (time 0.4)))\n"
            "(world (floor ice) (gravity 9.8))\n(waves (wave 1) (wave 2))\n");
    CHECK(clean.errors.empty());
    CHECK(clean.warnings.empty());

    // The note's three edits.
    CHECK(messages(run("(jump (height \"tall\") (buffer 3) (air (time 0.4)))\n"
                       "(world (floor ice) (gravity 9.8))")
                       .errors)
              .size() == 1);
    const auto far =
        run("(jump (height 1000) (buffer 3) (air (time 0.4)))\n(world (floor ice) (gravity 9.8))");
    REQUIRE(far.errors.size() == 1);
    CHECK(std::string(far.errors[0].message()) ==
          "'jump.height' is 1000, outside its tweak's range 16.0 to 320.0");
    CHECK(far.errors[0].line() == 1);
    const auto typo =
        run("(jump (hieght 120) (height 120) (buffer 3) (air (time 0.4)))\n"
            "(world (floor ice) (gravity 9.8))");
    CHECK(typo.errors.empty());
    CHECK(messages(typo.warnings) ==
          std::vector<std::string>{
              "'hieght' in 'jump' is read by no tweak; its tweaks read air, buffer, height"});

    // A tweak the data lacks keeps its default: a warning, never an error.
    const auto missing =
        run("(jump (height 120) (air (time 0.4)))\n(world (floor ice) (gravity 9.8))");
    CHECK(missing.errors.empty());
    CHECK(messages(missing.warnings) ==
          std::vector<std::string>{"the data has no 'jump.buffer': the tweak keeps its default"});

    // The data's own range and list win over the code's.
    const auto own =
        run("(jump (height 400 (range 16 500)) (buffer 3) (air (time 0.4)))\n"
            "(world (floor sand (one_of stone sand)) (gravity 9.8))");
    CHECK(own.errors.empty());
    const auto list =
        run("(jump (height 120) (buffer 3) (air (time 0.4)))\n(world (floor lava) (gravity 9.8))");
    CHECK(messages(list.errors) ==
          std::vector<std::string>{"'world.floor' is 'lava', not one of its tweak's stone, ice"});
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
    static void to(const Vec2& v, ValueList& out) {
        out.add(v.x);
        out.add(v.y);
    }
};

TEST_CASE("a tweak's value types, in the JSON, from Convert's values") {
    TweakRegistry reg;
    Tweak<Vec2> aim{"player.aim", Vec2{0, 1}, {}, reg};
    Tweak<std::vector<int>> waves{"waves", {}, {}, reg};
    CHECK(aim.value_types().size() == 2);
    CHECK(waves.value_types().empty());  // a list's count is the data's: no value types
    std::string json;
    write_json(reg, string_out(json));
    CHECK(json.find(R"j("type": "vec2", "values": ["float32", "float32"], )j") !=
          std::string::npos);
    CHECK(json.find(R"j("type": "list", "values": null, )j") != std::string::npos);
}

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
    CHECK(json.find(
              R"j("type": "vec2", "values": ["float32", "float32"], "default": "(0.0 1.0)")j") !=
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
                           .default_text = "(0.9 0.5 0.2)"}));
    for (const char* text : {"(cube (color 0.9 0.5 0.2))\n", "(cube (color 0.9 0.5))\n",
                             "(cube (color red 0.5 0.2))\n"}) {
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
    std::sort(bad.begin(), bad.end());  // in the registry's order otherwise
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

TEST_CASE("the metadata split and the color rule, for any value") {
    const Ref<Array> root = ok(read_string(
        "(width 2 (range 0 10) (desc \"w\"))\n(ball (kind ball) (pos 1 2))\n"
        "(tint 1 0.5 0 (range 0 1))\n(sky 0.2 0.4 0.9)\n(spot 1 0.5 0 (kind angle) (range 0 1))\n",
        "d.dta"));
    auto values = [&](const char* key) { return root->try_find(Symbol(key))->values(); };
    const ValueMetadata width = split_metadata(values("width"));
    REQUIRE(width.values.size() == 1);
    CHECK(width.values[0].try_int() == 2);
    CHECK(width.metadata.size() == 2);
    const ValueMetadata ball = split_metadata(values("ball"));  // (pos 1 2) is no metadata
    CHECK(ball.values.size() == 2);
    CHECK(ball.metadata.empty());

    CHECK(presented_kind(values("tint")) == "color");
    CHECK(presented_kind(values("sky")).empty());                 // no range 0..1
    CHECK(presented_kind(values("sky"), {}, {0, 1}) == "color");  // the code's range
    CHECK(presented_kind(values("spot")) == "angle");             // a kind given wins
    CHECK(presented_kind(values("width"), "length") == "length");

    // The same rule a tweak applies.
    TweakRegistry reg;
    Tweak<std::array<float, 3>> tint{"tint", {}, {}, reg};
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("d", root));
    CHECK(tint.kind() == presented_kind(values("tint")));
}

TEST_CASE("each tweak finding carries its subject as its path") {
    TweakRegistry reg;
    Tweak<float> gravity{"world.gravity", 9.8f, {.range = {0, 20}}, reg};
    Tweak<float> wind{"world.wind", 0.f, {}, reg};
    const TweakFindings f =
        check_tweaks(*ok(read_string("(world (gravity 30) (gravty 9))\n", "d.dta")), reg);
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].path() == "world.gravity");
    std::vector<std::string> paths;
    for (const Error& w : f.warnings) paths.emplace_back(w.path());
    std::sort(paths.begin(), paths.end());
    CHECK(paths == std::vector<std::string>{"world.gravty", "world.wind"});  // stray, missing
}
