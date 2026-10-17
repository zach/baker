// The tweaks as JSON, baker.tweaks/1.
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
          "{\n  \"schema\": \"baker.tweaks/1\",\n  \"tweaks\": [\n"
          R"j(    {"path": "player.lives", "type": "int32", "default": "3", "value": "3", )j"
          R"j("bound": false, "range": null, "step": 0.0, "desc": "", "kind": "", )j"
          R"j("group": "player", "dev_only": true, "source": null},)j"
          "\n"
          R"j(    {"path": "player.speed", "type": "float32", "default": "4.0", "value": "6.0", )j"
          R"j("bound": true, "range": [0.0, 10.0], "step": 0.5, "desc": "Run speed", )j"
          R"j("kind": "", "group": "player", "dev_only": false, )j"
          R"j("source": {"layer": "default", "file": "d.dta", "line": 1}})j"
          "\n  ]\n}\n");
}
