// The tweaks as JSON, baker.tweaks/1.1.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>

#include <string>
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
          "{\n  \"schema\": \"baker.tweaks/1.1\",\n  \"tweaks\": [\n"
          R"j(    {"path": "player.lives", "type": "int32", "default": "3", "value": "3", )j"
          R"j("bound": false, "range": null, "step": 0.0, "desc": "", "kind": "", )j"
          R"j("group": "player", "dev_only": true, "one_of": [], "source": null},)j"
          "\n"
          R"j(    {"path": "player.speed", "type": "float32", "default": "4.0", "value": "6.0", )j"
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
