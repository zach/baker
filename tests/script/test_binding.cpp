// Native function binding: signature deduction, conversions, overloads.
#include <doctest.h>
#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/std_script.hpp>
#include <baker/tools/std_tools.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include "alloc_counter.hpp"
#include "ok.hpp"

using namespace baker;
using namespace baker::tools;
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
    // A shipping build keeps no doc text: the status alone.
    CHECK(ref.find(kShipping ? "### `old_jump`\n\n- `{old_jump float32}` → float32\n\n*Native, "
                               "deprecated: use `jump`.*\n"
                             : "### `old_jump`\n\n- `{old_jump float32}` → float32\n\n*Native, "
                               "deprecated: use `jump`.* Jumps.\n") != std::string::npos);
    CHECK(ref.find("- `{jump float32 [unevaluated]}` → float32\n\n*Native, provisional.*\n") !=
          std::string::npos);
    if constexpr (!kShipping)
        CHECK(ref.find("## Globals\n\n### `$game_time`\n\nTicks since the level started.\n") !=
              std::string::npos);
}

namespace {

// A Context's own memory, counted apart from the default allocator's (operator new): a bump
// arena from one block, freed with it.
struct Counted {
    static constexpr size_t kBytes = size_t{16} << 20;
    std::unique_ptr<std::byte[]> block{new std::byte[kBytes]};
    size_t used = 0;
    size_t allocations = 0;
    Allocator allocator{[](void* user, size_t size, size_t align, baker::MemoryKind) -> void* {
                            auto& c = *static_cast<Counted*>(user);
                            c.used = (c.used + align - 1) & ~(align - 1);
                            REQUIRE(c.used + size <= kBytes);
                            void* p = c.block.get() + c.used;
                            c.used += size;
                            ++c.allocations;
                            return p;
                        },
                        [](void*, void*, size_t, baker::MemoryKind) {}, this};
};

}  // namespace

TEST_CASE("a call of more arguments than fit on the stack allocates nothing once warm") {
    Counted counted;
    Context ctx(counted.allocator);
    // An overload set evaluates every argument first; a declared function holds each.
    using I = int64_t;
    ok(ctx.func("s9", [](I a, I b, I c, I d, I e, I f, I g, I h, I i) {
        return a + b + c + d + e + f + g + h + i;
    }));
    ok(ctx.func("s9", [](double a, I b, I c, I d, I e, I f, I g, I h, I i) {
        return a + static_cast<double>(b + c + d + e + f + g + h + i);
    }));
    static constexpr ParamDescription kInt[] = {{.type = "int"}};
    static const ParamDescription kNine[] = {kInt[0], kInt[0], kInt[0], kInt[0], kInt[0],
                                             kInt[0], kInt[0], kInt[0], kInt[0]};
    static const OverloadDescription kDeclared[] = {{.params = kNine}};
    ok(ctx.declare("d9", kDeclared, [](Args& args) -> Expected<Node> {
        int64_t sum = 0;
        for (size_t i = 0; i < args.size(); ++i) sum += ok(args.eval(i)).try_int().value_or(0);
        return Node(sum);
    }));
    const auto set = ok(read_string("{s9 1 2 3 4 5 6 7 8 9}", "t"));
    const auto declared = ok(read_string("{d9 1 2 3 4 5 6 7 8 9}", "t"));
    CHECK(ok(ctx.eval((*set)[0])).try_int() == 45);  // the first call may set up the Context
    CHECK(ok(ctx.eval((*declared)[0])).try_int() == 45);
    const size_t before = counted.allocations;
    const test::AllocationScope scope;
    for (int k = 0; k < 1000; ++k) {
        (void)ctx.eval((*set)[0]);
        (void)ctx.eval((*declared)[0]);
    }
    if (test::counting_allocations()) CHECK(scope.count() == 0);  // none from the default
    CHECK(counted.allocations - before == 0);  // the spill stack is the Context's, and kept
    // Nested: a 9-argument call whose arguments make 9-argument calls, each in its own place.
    const auto nested =
        ok(read_string("{s9 {s9 1 1 1 1 1 1 1 1 1} 2 3 4 5 6 7 8 {s9 1 2 3 4 5 6 7 8 9}}", "t"));
    CHECK(ok(ctx.eval((*nested)[0])).try_int() == 9 + 35 + 45);
    // Wider than a block: its own block, kept as well.
    static const std::vector<ParamDescription> kSeventy(70, kInt[0]);
    static const OverloadDescription kWide[] = {{.params = kSeventy}};
    ok(ctx.declare("d70", kWide, [](Args& args) -> Expected<Node> {
        return Node(static_cast<int64_t>(args.size()));
    }));
    std::string wide = "{d70";
    for (int i = 0; i < 70; ++i) wide += " 1";
    const auto wide_call = ok(read_string(wide + "}", "t"));
    CHECK(ok(ctx.eval((*wide_call)[0])).try_int() == 70);
    const size_t warm = counted.allocations;
    for (int k = 0; k < 100; ++k) (void)ctx.eval((*wide_call)[0]);
    CHECK(counted.allocations == warm);
}

TEST_CASE(
    "a binding keeps a copy of an lvalue, the callable of an rvalue, and none when stateless") {
    struct Tracked {
        int* alive;
        int k;
        Tracked(int* a, int kk) : alive(a), k(kk) { ++*alive; }
        Tracked(const Tracked& o) : alive(o.alive), k(o.k) { ++*alive; }
        Tracked(Tracked&& o) noexcept : alive(o.alive), k(o.k) { ++*alive; }
        ~Tracked() { --*alive; }
        int64_t operator()(int64_t x) const { return x * k; }
    };
    int alive = 0;
    {
        Counted counted;
        Context ctx(counted.allocator);
        const Tracked times3(&alive, 3);
        ok(ctx.func("times3", times3));  // copied: the original stays the caller's
        CHECK(alive == 2);
        ok(ctx.func("times5", Tracked(&alive, 5)));  // moved from the temporary
        CHECK(alive == 3);
        auto owned = std::make_unique<int64_t>(7);  // move-only
        ok(ctx.func("plus7", [p = std::move(owned)](int64_t x) { return x + *p; }));
        // A stateless lambda costs the function's records alone; one with state, one more.
        size_t before = counted.allocations;
        ok(ctx.func("twice", [](int64_t x) { return 2 * x; }));
        const size_t stateless = counted.allocations - before;
        before = counted.allocations;
        ok(ctx.func("half", [k = 2.0](double x) { return x / k; }));
        CHECK(counted.allocations - before == stateless + 1);
        CHECK(ok(ctx.eval("{times3 2}")).try_int() == 6);
        CHECK(ok(ctx.eval("{times5 2}")).try_int() == 10);
        CHECK(ok(ctx.eval("{plus7 2}")).try_int() == 9);
        CHECK(ok(ctx.eval("{twice 21}")).try_int() == 42);
        CHECK(ok(ctx.eval("{half 3}")).try_float() == 1.5);
    }
    CHECK(alive == 0);  // the Context destroyed what it kept
}

namespace {
// Empty, but with a default constructor its author wrote: not a constant, so it is boxed.
struct WrittenDefault {
    WrittenDefault() {}  // NOLINT(modernize-use-equals-default): the case under test
    int64_t operator()(int64_t x) const { return x + 1; }
};
}  // namespace

TEST_CASE("an empty functor with a written default constructor binds") {
    static_assert(!detail::kStateless<WrittenDefault>);
    auto lambda = [](int64_t x) { return x; };
    static_assert(detail::kStateless<decltype(lambda)>);
    Context ctx;
    ok(ctx.func("next", WrittenDefault{}));
    CHECK(ok(ctx.eval("{next 41}")).try_int() == 42);
}
