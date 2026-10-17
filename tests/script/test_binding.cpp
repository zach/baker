// Native function binding: signature deduction, conversions, overloads.
#include <doctest.h>
#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/std_script.hpp>

#include <string>
#include <vector>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;

namespace {

struct Vec2 {
    double x = 0, y = 0;
};

}  // namespace

template <>
struct baker::Convert<Vec2> {
    static constexpr std::string_view name = "vec2";
    static bool from(const Values& v, Vec2& out) {
        return v.size() == 2 && v.read(0, out.x) && v.read(1, out.y);
    }
    static void to(const Vec2& v, ValueList& out) {
        out.add(v.x);
        out.add(v.y);
    }
};

namespace {
std::string text(const Node& n) {
    if (auto s = n.try_str()) return std::string(*s);
    return std::string(n.try_sym().value_or(Symbol()).str());
}
}  // namespace

TEST_CASE("parameter and return types are deduced from the signature") {
    Context ctx;
    ok(ctx.func("sym", [](Symbol s) { return s; }));
    ok(ctx.func("len", [](std::string_view s) { return s.size(); }));
    ok(ctx.func("shout", [](const std::string& s) { return s + "!"; }));
    ok(ctx.func("count", [](const Array& a) { return a.size(); }));
    ok(ctx.func("half", [](float f) { return f / 2; }));
    ok(ctx.func("neg", [](bool b) { return !b; }));
    ok(ctx.func("narrow", [](int8_t v) { return v; }));
    ok(ctx.func("type", [](Node n) { return Symbol(type_name(n.type())); }));
    ok(ctx.func("opt", [](int64_t a, std::optional<int64_t> b) { return a + b.value_or(100); }));
    ok(ctx.func("sum", [](Args a) -> Expected<int64_t> {
        int64_t s = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            auto v = a.get<int64_t>(i);
            if (!v) return Unexpected(std::move(v).error());
            s += *v;
        }
        return s;
    }));
    ok(ctx.func("first_rest",
                [](int64_t first, Args rest) { return first * 10 + int64_t(rest.size()); }));
    ok(ctx.func("ctx_var", [](Context& c, Symbol name) { return c.var(name).value(); }));
    ok(ctx.func("length", [](Vec2 v) { return v.x * v.x + v.y * v.y; }));
    ok(ctx.func("swap", [](Vec2 v) { return Vec2{v.y, v.x}; }));
    ok(ctx.func("nothing", [] {}));
    ok(ctx.func("checked", [](int64_t v) -> Expected<int64_t> {
        if (v < 0) return Unexpected(make_error(ErrorCode::Range, "negative"));
        return v;
    }));

    CHECK(ok(ctx.eval("{sym hello}")).as_sym() == Symbol("hello"));
    CHECK(ok(ctx.eval("{len \"four\"}")).as_int() == 4);
    CHECK(text(ok(ctx.eval("{shout \"hi\"}"))) == "hi!");
    CHECK(ok(ctx.eval("{count (1 2 3)}")).as_int() == 3);  // a literal: not copied
    CHECK(ok(ctx.eval("{do (($l (1 2))) {count $l}}")).as_int() == 2);
    CHECK(ok(ctx.eval("{count {if 1 (4 5 6 7) 0}}")).as_int() == 4);
    CHECK(ctx.eval("{count 3}").error().code() == ErrorCode::Type);
    CHECK(ctx.eval("{count {sym x}}").error().code() == ErrorCode::Type);
    CHECK(ok(ctx.eval("{half 1}")).as_float() == 0.5);
    CHECK(ok(ctx.eval("{half 0.2}")).as_float() == 0.1);  // a float result reads back as written
    CHECK(ok(ctx.eval("{neg 0}")).as_int() == 1);
    CHECK(ok(ctx.eval("{narrow 100}")).as_int() == 100);
    CHECK(ok(ctx.eval("{type {+ 1 1}}")).as_sym() == Symbol("int"));  // Node parameters evaluate
    CHECK(ok(ctx.eval("{opt 1}")).as_int() == 101);
    CHECK(ok(ctx.eval("{opt 1 2}")).as_int() == 3);
    CHECK(ok(ctx.eval("{sum}")).as_int() == 0);
    CHECK(ok(ctx.eval("{sum 1 2 {+ 3 4}}")).as_int() == 10);
    CHECK(ok(ctx.eval("{first_rest 4 a b}")).as_int() == 42);
    ctx.var("q") = 9;
    CHECK(ok(ctx.eval("{ctx_var q}")).as_int() == 9);
    CHECK(ok(ctx.eval("{length (3 4)}")).as_float() == 25.0);
    const Node swapped = ok(ctx.eval("{swap (1 2)}"));
    REQUIRE(swapped.try_array());
    CHECK(structurally_equal(*swapped.try_array(),
                             *ok(read_string("(2.0 1.0)"))->nodes()[0].try_array()));
    CHECK(ok(ctx.eval("{nothing}")).as_int() == 0);
    CHECK(ok(ctx.eval("{checked 5}")).as_int() == 5);

    const auto e = ctx.eval("\n{checked -1}", "c.dta");
    REQUIRE_FALSE(e);
    CHECK(e.error().message() == "negative");
    CHECK(e.error().line() == 2);  // located at the call
    const auto narrow = ctx.eval("{narrow 300}");
    REQUIRE_FALSE(narrow);
    CHECK(narrow.error().message() == "argument 1 of 'narrow' is int, not int8");
    const auto arity = ctx.eval("{opt}");
    REQUIRE_FALSE(arity);
    CHECK(arity.error().message() == "'opt' takes 1 to 2 arguments, given 0");
}

TEST_CASE("a string argument computed by a command lives until the function returns") {
    Context ctx;
    ok(ctx.func("make", [](int64_t n) { return std::string(size_t(n), 'x'); }));
    ok(ctx.func("len",
                [](std::string_view a, std::string_view b) { return a.size() * 100 + b.size(); }));
    CHECK(ok(ctx.eval("{len {make 40} {make 3}}")).as_int() == 4003);
}

TEST_CASE("Raw parameters are passed unevaluated") {
    Context ctx;
    int runs = 0;
    ok(ctx.func("bump", [&] { return ++runs; }));
    ok(ctx.func("twice", [](Raw<Node> body) -> Expected<Node> {
        auto r = body.eval();
        if (!r) return r;
        return body.eval();
    }));
    ok(ctx.func("quote", [](Raw<Node> n) { return *n; }));
    CHECK(ok(ctx.eval("{twice {bump}}")).as_int() == 2);
    CHECK(runs == 2);
    CHECK(ok(ctx.eval("{quote {bump}}")).type() == Type::Command);
    CHECK(runs == 2);
}

TEST_CASE("overload sets: registration order, exact types first, then Int as Float") {
    Context ctx;
    ok(ctx.func("f", [](int64_t) { return Symbol("int"); }));
    ok(ctx.func("f", [](double) { return Symbol("float"); }));
    ok(ctx.func("f", [](Symbol) { return Symbol("symbol"); }));
    ok(ctx.func("f", [](int64_t, int64_t) { return Symbol("two"); }));
    CHECK(ok(ctx.eval("{f 1}")).as_sym() == Symbol("int"));
    CHECK(ok(ctx.eval("{f 1.5}")).as_sym() == Symbol("float"));
    CHECK(ok(ctx.eval("{f x}")).as_sym() == Symbol("symbol"));
    CHECK(ok(ctx.eval("{f 1 2}")).as_sym() == Symbol("two"));

    ok(ctx.func("g", [](double, double) { return Symbol("float"); }));
    ok(ctx.func("g", [](Symbol, Symbol) { return Symbol("symbol"); }));
    CHECK(ok(ctx.eval("{g 1 2}")).as_sym() == Symbol("float"));  // promoted

    // Arguments are evaluated once, before choosing.
    int runs = 0;
    ok(ctx.func("bump", [&] { return ++runs; }));
    CHECK(ok(ctx.eval("{f {bump}}")).as_sym() == Symbol("int"));
    CHECK(runs == 1);

    // Ambiguity is a registration error, not a call error.
    const auto dup = ctx.func("f", [](int64_t v) { return v; });
    REQUIRE_FALSE(dup);
    CHECK(dup.error().message() == "'f' already has the overload (int)");
    const auto raw = ctx.func("f", [](Raw<Node> n) { return *n; });
    REQUIRE_FALSE(raw);
    CHECK(raw.error().message() == "'f' takes unevaluated arguments, so it cannot be overloaded");
}

TEST_CASE("documentation and status appear in the reference") {
    Context ctx;
    ok(ctx.func("old_jump", [](float h) { return h; })).doc("Jumps.").status(Deprecated, "jump");
    ok(ctx.func("jump", [](float h, std::optional<Raw<Node>> then) {
        return then ? h : -h;
    })).status(Provisional);
    ctx.global("game_time", 0, "Ticks since the level started.");
    const std::string ref = reference(ctx);
    CHECK(ref.find("### `old_jump`\n\n- `{old_jump float32}` → float32\n\n*Native, deprecated: use "
                   "`jump`.* Jumps.\n") != std::string::npos);
    CHECK(ref.find("- `{jump float32 [unevaluated]}` → float32\n\n*Native, provisional.*\n") !=
          std::string::npos);
    CHECK(ref.find("## Globals\n\n### `$game_time`\n\nTicks since the level started.\n") !=
          std::string::npos);
}
