// Debugging: the script call stack, error stacks, and script_backtrace (M6).
#include <doctest.h>
#include <baker/object.hpp>
#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/std_script.hpp>

#include "alloc_counter.hpp"

#include <string>
#include <vector>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;

namespace {

struct Guard : Object<Guard> {
    static void bind(Binder<Guard>& b) {
        b.method("patrol", [](Guard&, Context& ctx, int n) {
            return ctx.eval("{walk " + std::to_string(n) + "}");
        });
    }
};

Error eval_error(Context& ctx, std::string_view text) {
    auto r = ctx.eval(text, "boss.dta");
    REQUIRE_FALSE(r);
    return r.error();
}

}  // namespace

TEST_CASE("an error four calls deep carries its stack, innermost first") {
    Context ctx;
    const Error e = eval_error(ctx,
                               "{func d ($x) {% $x 0}}\n"  // 1
                               "{func c ($x) {d $x}}\n"    // 2
                               "{func b ($x)\n"            // 3
                               "  {c $x}}\n"               // 4
                               "{func a ($x) {b $x}}\n"    // 5
                               "{a 7}\n");                 // 6
    CHECK(e.message() == "'%' by zero");
    CHECK(e.line() == 1);
    REQUIRE(e.stack().size() == 5);
    const char* names[] = {"%", "d", "c", "b", "a"};
    const int32_t lines[] = {1, 2, 4, 5, 6};
    for (size_t i = 0; i < 5; ++i) {
        CHECK(e.stack()[i].function == names[i]);
        CHECK(e.stack()[i].file == "boss.dta");
        CHECK(e.stack()[i].line == lines[i]);
        CHECK(e.stack()[i].on.empty());
    }
    CHECK(describe(e) ==
          "baker: '%' by zero  (boss.dta:1)\n"
          "  in {%}  (boss.dta:1)\n"
          "  in {d}  (boss.dta:2)\n"
          "  in {c}  (boss.dta:4)\n"
          "  in {b}  (boss.dta:5)\n"
          "  in {a}  (boss.dta:6)");
    CHECK(to_string(e) == "baker: '%' by zero  (boss.dta:1)");  // still one line
    CHECK(ctx.top_frame() == nullptr);                          // every frame has unwound
}

TEST_CASE("a call that never starts is not on the stack") {
    Context ctx;
    const Error e = eval_error(ctx, "{func f () {nope}}\n{f}");
    CHECK(e.message() == "'nope' is not a function");
    REQUIRE(e.stack().size() == 1);
    CHECK(e.stack()[0].function == "f");
    CHECK(e.stack()[0].line == 2);
    // Nothing running: no stack at all.
    CHECK(eval_error(ctx, "{nope}").stack().empty());
}

TEST_CASE("methods and handlers name their object") {
    Context ctx;
    Guard g;
    REQUIRE(ctx.add(g, "guard"));
    REQUIRE(ctx.eval("{class Guard (alarm ($n) {+ $n nope})}"));
    const Error e = eval_error(ctx, "{guard alarm 1}");
    REQUIRE(e.stack().size() == 2);
    CHECK(e.stack()[0].function == "+");
    CHECK(e.stack()[1].function == "alarm");
    CHECK(e.stack()[1].on == "guard");
    CHECK(to_string(e.stack()[1]) == "{guard alarm}  (boss.dta:1)");
}

TEST_CASE("an error returned by a native function records the calls it leaves") {
    Context ctx;
    ok(ctx.func("explode", [](int) -> Expected<int> {
        return Unexpected(make_error(ErrorCode::Refused, "boom"));
    }));
    const Error e = eval_error(ctx, "{func f () {explode 1}}\n{f}");
    CHECK(e.message() == "boom");
    REQUIRE(e.stack().size() == 2);
    CHECK(e.stack()[0].function == "explode");
    CHECK(e.stack()[1].function == "f");
    CHECK(describe(e).find("\n  in {f}") != std::string::npos);
}

TEST_CASE("script_backtrace renders the live frames from inside a native function") {
    Context ctx;
    std::string seen;
    List<StackEntry> frames;
    REQUIRE(ctx.func("assert_here", [&](Context& c) {
        seen = script_backtrace();
        frames = c.backtrace();
    }));
    REQUIRE(ctx.eval("{func g () {assert_here}}\n{func f () {g}}\n{f}", "ai.dta"));
    CHECK(seen ==
          "in {assert_here}  (ai.dta:1)\n"
          "in {g}  (ai.dta:2)\n"
          "in {f}  (ai.dta:3)\n");
    CHECK(frames.size() == 3);
    CHECK(script_backtrace().empty());  // nothing running now
}

TEST_CASE("frames cross a native call back into script") {
    Context ctx;
    Guard g;
    REQUIRE(ctx.add(g, "guard"));
    REQUIRE(ctx.eval("{func walk ($n) {% $n 0}}"));
    const Error e = eval_error(ctx, "{guard patrol 3}");
    // The inner evaluation shares the frame list, so its error records the outer calls too.
    REQUIRE(e.stack().size() >= 3);
    CHECK(e.stack()[0].function == "%");
    CHECK(e.stack()[1].function == "walk");
    CHECK(e.stack().back().function == "patrol");
}

TEST_CASE("the frame list allocates nothing") {
    Context ctx;
    REQUIRE(ctx.eval("{func add1 ($a) {+ $a 1}}"));
    auto tree = read_string("{add1 {add1 1}}");
    REQUIRE(tree);
    const Node& call = (**tree)[0];
    REQUIRE(ctx.eval(call));
    test::AllocationScope scope;
    for (int i = 0; i < 100; ++i) REQUIRE(ctx.eval(call));
    CHECK(scope.count() == 0);
}
