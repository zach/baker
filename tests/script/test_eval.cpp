// Evaluation, variables, and the built-in vocabulary (M4).
#include <doctest.h>
#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/std_script.hpp>
#include <baker/tools/std_tools.hpp>

#include "alloc_counter.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "ok.hpp"

using namespace baker;
using namespace baker::tools;
using baker::test::ok;

namespace {

Node eval_ok(Context& ctx, std::string_view text) {
    auto r = ctx.eval(text, "test.dta");
    if (!r) FAIL(to_string(r.error()));
    return r ? *r : Node();
}

Error eval_error(Context& ctx, std::string_view text) {
    auto r = ctx.eval(text, "test.dta");
    REQUIRE_FALSE(r);
    return r.error();
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

}  // namespace

TEST_CASE("literals evaluate to themselves; commands run their function") {
    Context ctx;
    CHECK(eval_ok(ctx, "7").as_int() == 7);
    CHECK(eval_ok(ctx, "{+ 1 2}").as_int() == 3);
    CHECK(eval_ok(ctx, "{+ 1 {* 2 3}}").as_int() == 7);
    CHECK(eval_ok(ctx, "{+ 1 2.5}").as_float() == 3.5);  // Int read as Float, last
    CHECK(eval_ok(ctx, "{+ 1.5 2.5}").as_float() == 4.0);
    CHECK(eval_ok(ctx, "{/ 1 2}").as_float() == 0.5);
    CHECK(eval_ok(ctx, "{- 5}").as_int() == -5);
    CHECK(eval_ok(ctx, "{% -7 3}").as_int() == -1);
    CHECK(eval_ok(ctx, "{+ 9223372036854775807 1}").as_int() == INT64_MIN);  // wraps
    CHECK(eval_ok(ctx, "{== 1 1.0}").as_int() == 1);
    CHECK(eval_ok(ctx, "{== foo foo}").as_int() == 1);
    CHECK(eval_ok(ctx, "{!= \"a\" \"b\"}").as_int() == 1);
    CHECK(eval_ok(ctx, "{< 1 2.5}").as_int() == 1);
    CHECK(eval_ok(ctx, "{not 0}").as_int() == 1);
    CHECK(eval_ok(ctx, "{+ 1 2} {* 3 4}").as_int() == 12);  // the last value
    CHECK(eval_ok(ctx, "").as_int() == 0);
}

TEST_CASE("errors name the function and the call site") {
    Context ctx;
    Error e = eval_error(ctx, "\n{nope 1}");
    CHECK(e.message() == "'nope' is not a function");
    CHECK(e.file() == Symbol("test.dta"));
    CHECK(e.line() == 2);

    e = eval_error(ctx, "{not 1 2}");
    CHECK(e.message() == "'not' takes 1 argument, given 2");
    CHECK(e.line() == 1);

    e = eval_error(ctx, "\n\n{+ 1 two}");
    CHECK(e.message() == "no overload of '+' takes (int symbol); it has (int int) (float float)");
    CHECK(e.line() == 3);

    e = eval_error(ctx, "{not two}");
    CHECK(e.message() == "argument 1 of 'not' is symbol, not bool");

    e = eval_error(ctx, "{% 1 0}");
    CHECK(e.message() == "'%' by zero");
    CHECK(e.line() == 1);

    CHECK(eval_error(ctx, "{}").message() == "empty command {}");
    CHECK(eval_error(ctx, "$nobody").message() == "$nobody is not set");
    CHECK(eval_error(ctx, "{+ $nobody 1}").line() == 1);
}

TEST_CASE("if and set are ordinary functions, and the evaluator knows no function by name") {
    Context ctx;
    CHECK(ctx.has_func("if"));
    CHECK(ctx.has_func("set"));
    // The evaluator (context.cpp) names no function: everything goes through the table.
    const std::string src = slurp(BAKER_SOURCE_DIR "/src/script/context.cpp");
    REQUIRE_FALSE(src.empty());
    for (const char* name : {"\"if\"", "\"set\"", "\"do\"", "\"and\"", "\"or\"", "\"func\""})
        CHECK_MESSAGE(src.find(name) == std::string::npos, name);
    // Registering either again is an ordinary registration, refused as for any function
    // that takes unevaluated arguments.
    const auto r = ctx.func("if", [](int64_t a, int64_t b) { return a * b; });
    REQUIRE_FALSE(r);
    CHECK(r.error().message() == "'if' takes unevaluated arguments, so it cannot be overloaded");
}

TEST_CASE("the untaken branch of an if is never evaluated") {
    Context ctx;
    int taken = 0, untaken = 0;
    REQUIRE(ctx.func("taken", [&] { return ++taken; }));
    REQUIRE(ctx.func("untaken", [&] { return ++untaken; }));
    CHECK(eval_ok(ctx, "{if 1 {taken} {untaken}}").as_int() == 1);
    CHECK(eval_ok(ctx, "{if 0 {untaken} {taken}}").as_int() == 2);
    CHECK(eval_ok(ctx, "{if 0 {untaken}}").as_int() == 0);
    CHECK(eval_ok(ctx, "{and 1 0 {untaken}}").as_int() == 0);
    CHECK(eval_ok(ctx, "{or 0 1 {untaken}}").as_int() == 1);
    CHECK(taken == 2);
    CHECK(untaken == 0);
}

TEST_CASE("variables: set, read from C++, and dynamic scope through do") {
    Context ctx;
    ctx.var("game_time") = 500;
    CHECK(eval_ok(ctx, "{+ $game_time 1}").as_int() == 501);
    CHECK(eval_ok(ctx, "{set $game_time 7}").as_int() == 7);
    CHECK(ctx.var("game_time")->as_int() == 7);
    CHECK(ctx.var("game_time").is_set());
    CHECK_FALSE(ctx.var("never").is_set());
    CHECK(ctx.var("never").value().as_int() == 0);

    // do binds for its body and restores after; ($b 2) binds to a value.
    ctx.var("a") = 1;
    CHECK(eval_ok(ctx, "{do ($a ($b 2)) {set $a 10} {+ $a $b}}").as_int() == 12);
    CHECK(ctx.var("a")->as_int() == 1);
    CHECK_FALSE(ctx.var("b").is_set());

    // Restored on error too.
    CHECK(eval_error(ctx, "{do (($a 5)) {nope}}").message() == "'nope' is not a function");
    CHECK(ctx.var("a")->as_int() == 1);

    // Dynamic, not lexical: a function sees its caller's bindings.
    eval_ok(ctx, "{func peek () $a}");
    CHECK(eval_ok(ctx, "{do (($a 7)) {peek}}").as_int() == 7);
    CHECK(eval_ok(ctx, "{peek}").as_int() == 1);

    // set needs a variable.
    CHECK(eval_error(ctx, "{set a 1}").message() ==
          "argument 1 of 'set' must be a $variable, not symbol");
}

TEST_CASE("script functions: arguments evaluate in the caller's scope, then bind") {
    Context ctx;
    eval_ok(ctx, "{func add1 ($a) {+ $a 1}}");
    CHECK(eval_ok(ctx, "{add1 41}").as_int() == 42);
    ctx.var("a") = 10;
    CHECK(eval_ok(ctx, "{add1 {+ $a 1}}").as_int() == 12);  // $a is still the caller's 10
    CHECK(ctx.var("a")->as_int() == 10);
    CHECK(eval_error(ctx, "{add1}").message() == "'add1' takes 1 argument, given 0");
    eval_ok(ctx, "{func fact ($n) {if {< $n 2} 1 {* $n {fact {- $n 1}}}}}");
    CHECK(eval_ok(ctx, "{fact 10}").as_int() == 3628800);
    eval_ok(ctx, "{func forever () {forever}}");
    const Error e = eval_error(ctx, "{forever}");
    CHECK(e.code() == ErrorCode::Limit);
    CHECK(e.message().starts_with("calls nested too deep ("));
    CHECK(eval_ok(ctx, "{fact 20}").as_int() == 2432902008176640000);
    ctx.set_stack_budget(4 * 1024);  // the limit is the stack used, not a call count
    CHECK(eval_error(ctx, "{fact 20}").code() == ErrorCode::Limit);
}

TEST_CASE("a script stub replaced by a native function runs unchanged call sites") {
    Context ctx;
    std::vector<std::string> notices;
    ctx.set_notice_sink(
        [](void* user, const Error& e) {
            static_cast<std::vector<std::string>*>(user)->emplace_back(e.message());
        },
        &notices);
    auto tree =
        ok(read_string("{func boss.find_cover ($p) (status stub) (doc \"placeholder\") {+ $p 1}}\n"
                       "(encounter (cover {boss.find_cover 10}))\n",
                       "boss.dta"));
    REQUIRE(ctx.run(tree->nodes()));
    const Array& encounter = *tree->try_find("encounter");
    CHECK(encounter.find_int("cover") == 11);  // the stub
    CHECK(reference(ctx).find("*Script, stub.* placeholder") != std::string::npos);

    REQUIRE(ctx.func("boss.find_cover", [](int64_t p) { return p * 100; }));
    CHECK(encounter.find_int("cover") == 1000);  // same call site, now native
    REQUIRE(notices.size() == 1);
    CHECK(notices[0] == "native 'boss.find_cover' replaces the script definition");
    CHECK(reference(ctx).find(
              "### `boss.find_cover`\n\n- `{boss.find_cover int}` → int\n\n*Native.*") !=
          std::string::npos);

    // An arity mismatch is noted.
    eval_ok(ctx, "{func two ($a $b) $a}");
    REQUIRE(ctx.func("two", [](int64_t a) { return a; }));
    CHECK(notices.back() ==
          "native 'two' replaces the script definition; the script took 2 arguments, the native "
          "takes 1 argument");
}

TEST_CASE("a script func cannot silently replace a native one; override can, and logs") {
    Context ctx;
    std::vector<std::string> notices;
    ctx.set_notice_sink(
        [](void* user, const Error& e) {
            static_cast<std::vector<std::string>*>(user)->emplace_back(e.message());
        },
        &notices);
    REQUIRE(ctx.func("greet", [] { return 1; }));
    CHECK(eval_error(ctx, "{func greet () 2}").message() ==
          "'greet' is a native function; write {override greet ...} to replace it");
    CHECK(eval_ok(ctx, "{greet}").as_int() == 1);
    eval_ok(ctx, "{override greet () 2}");
    CHECK(eval_ok(ctx, "{greet}").as_int() == 2);
    REQUIRE(notices.size() == 1);
    CHECK(notices[0] == "script {override} replaces native 'greet'");
}

TEST_CASE("typed lookups in baker::data evaluate commands and variables") {
    Context ctx;
    ctx.var("difficulty") = 3;
    auto tree = ok(
        read_string("(enemy (hp {* 100 $difficulty}) (speed $difficulty) (name {if 1 grunt boss}) "
                    "(sizes {+ 1 1} 3) (raw {+ 1 2}))",
                    "enemy.dta"));
    const Array& enemy = *tree->try_find("enemy");
    CHECK(enemy.find_int("hp") == 300);
    CHECK(enemy.find_float("speed") == 3.0);
    CHECK(enemy.find_sym("name") == Symbol("grunt"));
    CHECK(enemy.find<std::vector<int>>("sizes") == std::vector<int>{2, 3});
    CHECK(tree->get<int>("enemy.hp") == 300);
    CHECK(tree->get<int>("enemy.missing", 5) == 5);
    // Reading as a Node stays raw.
    CHECK(ok(enemy.find<Node>("raw")).type() == Type::Command);
    // A value that evaluates to the wrong type is reported as what it evaluated to.
    const auto bad = enemy.find<int>("name");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().message() == "'name' is symbol, not int");
    CHECK(bad.error().line() == 1);
}

TEST_CASE("lookups use the thread's current Context") {
    auto tree = ok(read_string("(x $level)", "x.dta"));
    Context first;
    first.var("level") = 1;
    Context second;
    second.var("level") = 2;
    CHECK(Context::current() == &first);  // the first one created on the thread
    CHECK(tree->find_int("x") == 1);
    {
        Context::Scope scope(second);
        CHECK(tree->find_int("x") == 2);
    }
    CHECK(tree->find_int("x") == 1);
    // While a Context evaluates, it is current.
    REQUIRE(second.func("read_x", [&] { return tree->find_int("x"); }));
    CHECK(ok(second.eval("{read_x}")).as_int() == 2);
}

TEST_CASE("a lookup with no current Context is a clean error") {
    auto tree = ok(read_string("(x $level)", "x.dta"));
    std::optional<Context> ctx(std::in_place);
    ctx.reset();  // installs the evaluator, then leaves no Context current
    REQUIRE(Context::current() == nullptr);
    const auto r = tree->find<int>("x");
    REQUIRE_FALSE(r);
    CHECK(r.error().message() == "a var value needs a current baker::Context on this thread");
}

TEST_CASE("an error returned by a function comes back from eval") {
    Context ctx;
    auto tree = ok(read_string("(cfg (width 2))", "cfg.dta"));
    REQUIRE(ctx.func("height", [&] { return tree->try_find("cfg")->find_int("height"); }));
    const auto r = ctx.eval("{+ 1 {height}}");
    REQUIRE_FALSE(r);
    CHECK(r.error().message() == "'height' not found");
    CHECK(eval_ok(ctx, "{+ 1 1}").as_int() == 2);  // and the Context still works
}

TEST_CASE("calling a native function allocates nothing") {
    if (!test::counting_allocations()) return;
    Context ctx;
    REQUIRE(ctx.func("add", [](int64_t a, int64_t b) { return a + b; }));
    ctx.var("x") = 4;
    auto tree =
        ok(read_string("{add 1 2} {+ 1.5 $x} {if {< $x 5} {add $x 1} 0} {do (($y 1)) {set $y 2}}"));
    for (int i = 0; i < 3; ++i) REQUIRE(ctx.run(tree->nodes()));  // warm the stacks
    test::AllocationScope scope;
    int64_t sum = 0;
    for (int i = 0; i < 100; ++i) sum += *ok(ctx.eval((*tree)[0])).as_int();
    for (int i = 0; i < 100; ++i) REQUIRE(ctx.run(tree->nodes()));
    CHECK(scope.count() == 0);
    CHECK(sum == 300);
}

TEST_CASE("a function redefined while it runs finishes its old body") {
    Context ctx;
    REQUIRE(ctx.func("redefine", [](Context& c) { return c.eval("{func f () 2}").has_value(); }));
    REQUIRE(ctx.eval("{func f () {redefine} {func g () 9} 3}"));
    CHECK(*ctx.eval("{f}")->try_int() == 3);
    CHECK(*ctx.eval("{f}")->try_int() == 2);
    REQUIRE(ctx.eval("{func h () {func h () 1} 4}"));
    CHECK(*ctx.eval("{h}")->try_int() == 4);
    CHECK(*ctx.eval("{h}")->try_int() == 1);
}

TEST_CASE("run_autorun restores what the last run's commands defined") {
    Context ctx;
    REQUIRE(ctx.func("add", [](int64_t a, int64_t b) { return a + b; }));
    ok(ctx.eval("{func console_fn () 7}"));  // the host's own, outside autorun
    auto commands = [](const char* text) {
        auto root = ok(read_string(text, "layer.dta"));
        return std::vector<Node>(root->begin(), root->end());
    };
    const auto base = commands("{func greet () hello} {class Shop (sell () base)}");
    const auto dlc = commands(
        "{func greet () dlc_hello} {func dlc_bonus ($x) {* $x 2}} {override add ($a $b) 0}"
        "{extend Shop (sell () dlc) (haggle () yes)}");
    std::vector<Node> both = base;
    both.insert(both.end(), dlc.begin(), dlc.end());

    CHECK(ctx.run_autorun(both).empty());
    CHECK(ok(ctx.eval("{dlc_bonus 4}")).try_int() == 8);
    CHECK(ok(ctx.eval("{add 2 3}")).try_int() == 0);
    CHECK(to_string(ok(ctx.eval("{greet}"))) == "dlc_hello");
    ok(ctx.eval("{new Shop s}"));
    CHECK(to_string(ok(ctx.eval("{s sell}"))) == "dlc");

    // The DLC is uninstalled: its commands are no longer in the stack's autorun.
    CHECK(ctx.run_autorun(base).empty());
    CHECK_FALSE(ctx.eval("{dlc_bonus 4}"));                // gone
    CHECK(ok(ctx.eval("{add 2 3}")).try_int() == 5);       // the native is back
    CHECK(to_string(ok(ctx.eval("{greet}"))) == "hello");  // the lower layer's
    CHECK(to_string(ok(ctx.eval("{s sell}"))) == "base");  // the class's handlers
    CHECK_FALSE(ctx.eval("{s haggle}"));
    CHECK(ok(ctx.eval("{console_fn}")).try_int() == 7);  // never autorun's

    // Nothing left: everything autorun defined is undone.
    CHECK(ctx.run_autorun({}).empty());
    CHECK_FALSE(ctx.eval("{greet}"));

    // A failing command is reported, and the rest still run.
    const auto mixed = commands("{nope} {func after () 1}");
    const List<Error> failed = ctx.run_autorun(mixed);
    REQUIRE(failed.size() == 1);
    CHECK(ok(ctx.eval("{after}")).try_int() == 1);
}
