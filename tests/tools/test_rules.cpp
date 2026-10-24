// Rules between values, declared in shapes and evaluated by baker::tools.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>
#include <baker/std_script.hpp>
#include <baker/tools/rules.hpp>

#include <cstdio>
#include <string>
#include <vector>
#include "dev_only.hpp"
#include "ok.hpp"
#include "temp_dir.hpp"

using namespace baker;
using baker::test::ok;

namespace {

std::vector<std::string> texts(const List<Error>& list) {
    std::vector<std::string> out;
    for (const Error& e : list) out.emplace_back(to_string(e));
    return out;
}

// The rules of `text`'s shapes over `text` itself.
shape::Report ruled(Context& ctx, std::string_view text) {
    const auto tree = ok(read_string(text, "t.dta"));
    shape::Set shapes;
    shape::Report report;
    shapes.add(*tree, report);
    REQUIRE(report.errors.empty());
    tools::check_rules(*tree, shapes, ctx, report);
    return report;
}

const char* const kUnits =
    "(shapes (unit (at units) (keyed) (max_hp int) (start_hp int)\n"
    "   (rule \"starts at or below its maximum\" {<= $start_hp $max_hp})\n"
    "   (rule \"has a name\" {!= $name \"\"} (warn))))\n";

}  // namespace

TEST_CASE("a rule that fails is an error with the values it read; (warn) is a warning") {
    Context ctx;
    const shape::Report good = ruled(ctx, std::string(kUnits) +
                                              "(units (grunt (max_hp 10) (start_hp 10) "
                                              "(name \"Grunt\")))\n");
    CHECK(good.errors.empty());
    CHECK(good.warnings.empty());
    const shape::Report bad = ruled(
        ctx, std::string(kUnits) + "(units\n (grunt (max_hp 10) (start_hp 12) (name \"\")))\n");
    CHECK(texts(bad.errors) ==
          std::vector<std::string>{"baker: starts at or below its maximum ($start_hp 12, "
                                   "$max_hp 10)  (t.dta:5, at units/grunt)"});
    CHECK(texts(bad.warnings) ==
          std::vector<std::string>{"baker: has a name ($name \"\")  (t.dta:5, at units/grunt)"});
}

TEST_CASE("a rule that does not evaluate is an error naming it and why") {
    Context ctx;
    const shape::Report r = ruled(ctx,
                                  "(shapes (unit (at units) (keyed)\n"
                                  "   (rule \"is balanced\" {balance_of $cost})\n"
                                  "   (rule \"is a number\" {* $cost 1.5})))\n"
                                  "(units (grunt (cost 3)))\n");
    REQUIRE(r.errors.size() == 2);
    CHECK(texts(r.errors)[0].find("rule \"is balanced\" does not evaluate: ") != std::string::npos);
    CHECK(texts(r.errors)[0].find("balance_of") != std::string::npos);
    // An integral number reads as true or false; 4.5 does not.
    CHECK(texts(r.errors)[1].find(
              "rule \"is a number\" does not evaluate: it gives 4.5, not true or false") !=
          std::string::npos);
    // A field the record lacks leaves its variable unset, and reading it fails.
    const shape::Report missing = ruled(ctx,
                                        "(shapes (unit (at units) (keyed)\n"
                                        "   (rule \"is cheap\" {< $cost 5})))\n"
                                        "(units (grunt (hp 3)))\n");
    REQUIRE(missing.errors.size() == 1);
    CHECK(texts(missing.errors)[0].find("rule \"is cheap\" does not evaluate") !=
          std::string::npos);
}

TEST_CASE("a rule reads another table by a key field, and sums a table") {
    Context ctx;
    const char* const shapes =
        "(shapes\n"
        "   (card (at cards) (keyed) (tier (key_of tiers)) (power float)\n"
        "      (rule \"within 10% of its tier's ideal\"\n"
        "            {and {>= $power {* 0.9 {data tiers $tier ideal}}}\n"
        "                 {<= $power {* 1.1 {data tiers $tier ideal}}}}))\n"
        "   (loot (rule \"weights sum to 100\" {== {data.sum loot weight} 100})))\n";
    const shape::Report good = ruled(ctx, std::string(shapes) +
                                              "(tiers (basic (ideal 10)) (epic (ideal 50)))\n"
                                              "(cards (spark (tier basic) (power 10.5)) "
                                              "(nova (tier epic) (power 48)))\n"
                                              "(loot (common (weight 60)) (rare (weight 40)))\n");
    CHECK(texts(good.errors).empty());
    const shape::Report bad = ruled(ctx, std::string(shapes) +
                                             "(tiers (basic (ideal 10)) (epic (ideal 50)))\n"
                                             "(cards (spark (tier basic) (power 30)))\n"
                                             "(loot (common (weight 60)) (rare (weight 30)))\n");
    CHECK(texts(bad.errors) ==
          std::vector<std::string>{
              "baker: within 10% of its tier's ideal ($power 30, $tier basic)  (t.dta:8, at "
              "cards/spark)",
              "baker: weights sum to 100  (t.dta:9, at loot)"});
    // {data} outside a rule's check reads nothing.
    tools::declare_rule_functions(ctx);
    CHECK_FALSE(ctx.eval("{data tiers basic ideal}", "t"));
}

TEST_CASE("check_rules puts the Context's own variables back") {
    Context ctx;
    ctx.var(Symbol("max_hp")) = Node(int64_t{99});
    (void)ruled(ctx, std::string(kUnits) + "(units (grunt (max_hp 10) (start_hp 12)))\n");
    CHECK(ctx.var(Symbol("max_hp")).value().try_int() == 99);
    CHECK_FALSE(ctx.var(Symbol("start_hp")).is_set());
}

BAKER_DEV_TEST_CASE("a layer adds or softens a rule, and a live edit that breaks one warns") {
    test::TempDir dir;
    std::vector<std::string> reported;  // before the Config, whose handler appends to it
    Context ctx;
    Config cfg({.tweaks = nullptr});
    cfg.set_warning_handler([&](const Error& e) { reported.emplace_back(to_string(e)); });
    cfg.add_validator(tools::rules_validator(ctx));
    const std::string units = std::string(kUnits) +
                              "(units (grunt (max_hp 10) (start_hp %d) "
                              "(name \"G\")))\n";
    auto write = [&](int start) {
        char text[512];
        std::snprintf(text, sizeof text, units.c_str(), start);
        return dir.write("base.dta", text);
    };
    REQUIRE(cfg.push_layer("base", write(10)));
    CHECK(reported.empty());
    // The designer saves a start above the maximum: the reload warns.
    write(20);
    REQUIRE(cfg.reload());
    REQUIRE(reported.size() == 1);
    CHECK(reported[0].find("starts at or below its maximum ($start_hp 20, $max_hp 10)") !=
          std::string::npos);
    // A layer softens it to a warning, and adds a rule of its own that fails.
    reported.clear();
    REQUIRE(cfg.push_layer(
        "mod", ok(read_string("(shapes (unit\n"
                              "   (rule \"starts at or below its maximum\" {<= $start_hp "
                              "$max_hp} (warn))\n"
                              "   (rule \"is modest\" {< $max_hp 5})))\n",
                              "mod.dta"))));
    CHECK(reported.size() == 2);
}

TEST_CASE("without the game, check() reports a rule's game functions as demand") {
    Context tool;
    tools::declare_rule_functions(tool);
    const auto tree =
        ok(read_string("(shapes (card (at cards) (keyed)\n"
                       "   (rule \"fair\" {< {power_of $name} {data caps power}})))\n"
                       "(caps (power 10))\n(cards (spark (name spark)))\n",
                       "t.dta"));
    const CheckReport report = check(*tree, tool);
    std::vector<std::string> wanted;
    for (const CheckReport::Demand& d : report.demand) wanted.emplace_back(d.name.str());
    CHECK(wanted == std::vector<std::string>{"power_of"});  // `data` is known
}
