// The tweaks as JSON, baker.tweaks/1.2.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>

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
          "{\n  \"schema\": \"baker.tweaks/1.2\",\n  \"tweaks\": [\n"
          R"j(    {"path": "player.lives", "type": "int32", "values": ["int32"], "default": "3", )j"
          R"j("value": "3", )j"
          R"j("bound": false, "range": null, "step": 0.0, "desc": "", "kind": "", )j"
          R"j("group": "player", "dev_only": true, "one_of": [], "source": null},)j"
          "\n"
          R"j(    {"path": "player.speed", "type": "float32", "values": ["float32"], )j"
          R"j("default": "4.0", "value": "6.0", )j"
          R"j("bound": true, "range": [0.0, 10.0], "step": 0.5, "desc": "Run speed", )j"
          R"j("kind": "", "group": "player", "dev_only": false, "one_of": [], )j"
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
    CHECK(json.find(R"j("one_of": ["light", "heavy"], "source")j") != std::string::npos);
    CHECK(json.find(R"j("one_of": ["light", "heavy", "kick"], "source")j") != std::string::npos);
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
