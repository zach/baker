// The tweaks as JSON, baker.tweaks/1.5, and the data held to them.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;

namespace {
std::vector<std::string> messages(const List<Error>& list) {
    std::vector<std::string> out;
    for (const Error& e : list) out.emplace_back(e.message());
    return out;
}
}  // namespace

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

TEST_CASE("the tweaks as JSON: type, default, value, metadata, and source") {
    TweakRegistry reg;
    Tweak<float> speed{
        "player.speed", 4.0f, {.range = {0, 10}, .step = 0.5, .desc = "Run speed"}, reg};
    Tweak<int> lives{"player.lives", 3, {.dev_only = true}, reg};
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("default", ok(read_string("(player (speed 6))\n", "d.dta"))));
    std::string json;
    write_json(reg, string_out(json));
    CHECK(
        json ==
        "{\n  \"schema\": \"baker.tweaks/1.5\",\n  \"tweaks\": [\n"
        R"j(    {"path": "player.lives", "type": "int32", "values": ["int32"], "count": "exact", )j"
        R"j("element": null, "length": null, "default": "3", )j"
        R"j("value": "3", )j"
        R"j("bound": false, "range": null, "step": 0.0, "desc": "", "kind": "", )j"
        R"j("group": "player", "dev_only": true, "one_of": [], "choices": [], "source": null},)j"
        "\n"
        R"j(    {"path": "player.speed", "type": "float32", "values": ["float32"], "count": "exact", )j"
        R"j("element": null, "length": null, "default": "4.0", "value": "6.0", )j"
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

TEST_CASE("a tweak's value types, in the JSON, from Convert's values") {
    TweakRegistry reg;
    Tweak<Vec2> aim{"player.aim", Vec2{0, 1}, {}, reg};
    Tweak<std::vector<int>> waves{"waves", {}, {}, reg};
    CHECK(aim.value_types().size() == 2);
    REQUIRE(waves.value_types().size() == 1);  // a list: any number of its one type
    CHECK(waves.value_types()[0] == "int32");
    CHECK(waves.value_count() == ValueCount::Any);
    CHECK(aim.value_count() == ValueCount::Exact);
    std::string json;
    write_json(reg, string_out(json));
    CHECK(json.find(R"j("type": "vec2", "values": ["float32", "float32"], "count": "exact", )j") !=
          std::string::npos);
    CHECK(json.find(R"j("type": "list", "values": ["int32"], "count": "any", )j") !=
          std::string::npos);
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
    CHECK(paths == std::vector<std::string>{"world.wind", "world.gravty"});  // missing, stray
}

TEST_CASE(
    "tweak findings come in check --baker-json's order, whatever order the tweaks were registered "
    "in") {
    const auto findings = [](bool reversed) {
        TweakRegistry reg;
        std::vector<std::unique_ptr<Tweak<int>>> tweaks;
        const char* paths[] = {"b.speed", "a.speed", "c.size", "a.count", "b.mass"};
        for (size_t i = 0; i < 5; ++i)
            tweaks.push_back(std::make_unique<Tweak<int>>(paths[reversed ? 4 - i : i], 0,
                                                          TweakMeta{.range = {0, 10}}, reg));
        const TweakFindings f = check_tweaks(
            *ok(read_string(
                "(a (speed 11) (count 12) (cuont 1)) (b (speed 13)) (c (size 1) (sise 2))\n",
                "d.dta")),
            reg);
        return std::pair(messages(f.errors), messages(f.warnings));
    };
    const auto forward = findings(false);
    CHECK(forward == findings(true));
    REQUIRE(forward.first.size() == 3);
    CHECK(forward.first[0].find("'a.count'") != std::string::npos);
    CHECK(forward.first[2].find("'b.speed'") != std::string::npos);
    REQUIRE(forward.second.size() == 3);
    CHECK(forward.second[0].find("'b.mass'") != std::string::npos);  // missing: no place
    CHECK(forward.second[1].find("'cuont'") != std::string::npos);   // read by no tweak
    CHECK(forward.second[2].find("'sise'") != std::string::npos);
}
