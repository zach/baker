// The script runtime's costs: Context::call's commands, filled in place and pooled up to
// a message's most fields; thread tasks' captures swapped in and out; globals indexed.
#include <doctest.h>
#include <baker/object.hpp>
#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/task.hpp>
#include <baker/tools/std_tools.hpp>
#include <baker/write.hpp>

#include "alloc_counter.hpp"
#include "ok.hpp"

#include <string>
#include <vector>

using namespace baker;
using baker::test::ok;

namespace {

struct Mob : Object<Mob> {
    int64_t sum = 0;
    static void bind(Binder<Mob>& b) { b.property("sum", &Mob::sum); }
};

struct Msg8 {
    static constexpr auto name = "m8";
    int64_t a, b, c, d, e, f, g, h;
};
static_assert(detail::field_count<Msg8>() == detail::kMaxMessageFields);

// {class Mob (wN ($a1 ... $aN) {+ 0 $a1 ... $aN})} for N in 0..9, and m8.
std::string mob_class() {
    std::string text = "{class Mob";
    for (int n = 0; n <= 9; ++n) {
        text += " (w" + std::to_string(n) + " (";
        std::string sum = "{+ 0 0";
        for (int i = 1; i <= n; ++i) {
            text += " $a" + std::to_string(i);
            sum += " $a" + std::to_string(i);
        }
        text += ") " + sum + "})";
    }
    return text + " (m8 ($a $b $c $d $e $f $g $h) {+ $a $b $c $d $e $f $g $h})}";
}

// The methods w0..w9, named once: a std::string built per call allocates in MSVC's debug
// library, which the counting scopes below would see.
const Symbol& method(int n) {
    static const Symbol names[] = {Symbol("w0"), Symbol("w1"), Symbol("w2"), Symbol("w3"),
                                   Symbol("w4"), Symbol("w5"), Symbol("w6"), Symbol("w7"),
                                   Symbol("w8"), Symbol("w9")};
    return names[n];
}

// Calls wN on `mob` with the arguments 1..N, through the span overload.
Expected<Node> call_n(Context& ctx, Mob& mob, int n) {
    Node args[9];
    for (int i = 0; i < n; ++i) args[i] = Node(int64_t{i + 1});
    return ctx.call(mob.node(), method(n), std::span<const Node>(args, static_cast<size_t>(n)));
}

}  // namespace

TEST_CASE("Context::call with up to 8 arguments allocates nothing once warm; 9 allocates") {
    Context ctx;
    Mob mob;
    ok(ctx.add(mob, "mob"));
    ok(ctx.eval(mob_class()));
    for (int n = 0; n <= 9; ++n)
        for (int k = 0; k < 3; ++k) CHECK(ok(call_n(ctx, mob, n)).try_int() == n * (n + 1) / 2);
    if (!test::counting_allocations()) return;
    // Failures are counted and checked after each scope, so the scope sees only the calls.
    int failed = 0;
    for (int n = 0; n <= 8; ++n) {
        test::AllocationScope scope;
        for (int k = 0; k < 100; ++k) failed += !call_n(ctx, mob, n);
        const size_t count = scope.count();
        CHECK_MESSAGE(count == 0, n, " arguments");
    }
    // The largest pooled count, through the variadic overload as a message's fields go.
    {
        test::AllocationScope scope;
        for (int k = 0; k < 100; ++k) {
            auto r = ctx.call(mob, Symbol("w8"), 1, 2, 3, 4, 5, 6, 7, 8);
            failed += !r || r->try_int() != 36;
        }
        const size_t count = scope.count();
        CHECK(count == 0);
    }
    // One past it: each call allocates its command, as Context::call documents, and only that.
    test::AllocationScope scope;
    for (int k = 0; k < 100; ++k) failed += !call_n(ctx, mob, 9);
    const size_t count = scope.count();
    CHECK(count == 100);
    CHECK(failed == 0);
}

TEST_CASE("a message of 8 fields to a script handler allocates nothing once warm") {
    Context ctx;
    Mob mob;
    ok(ctx.add(mob, "mob"));
    ok(ctx.eval(mob_class()));
    for (int k = 0; k < 3; ++k) CHECK(ok(mob.handle(Msg8{1, 2, 3, 4, 5, 6, 7, 8})).try_int() == 36);
    if (!test::counting_allocations()) return;
    int failed = 0;
    test::AllocationScope scope;
    for (int k = 0; k < 100; ++k) failed += !mob.handle(Msg8{1, 2, 3, 4, 5, 6, 7, 8});
    const size_t count = scope.count();
    CHECK(count == 0);
    CHECK(failed == 0);
}

TEST_CASE("a command Context::call fills equals the array make_copy gives its nodes") {
    Context ctx;
    int seen = 0;
    // Compares the command it is called by with a copy made the general way.
    REQUIRE(ctx.func("probe", [&seen](Args a) {
        const Array& got = a.command();
        const Ref<Array> want = Array::make_copy(got.nodes());
        CHECK(structurally_equal(got, *want));
        CHECK(got.height() == want->height());
        CHECK(got.has_marker() == want->has_marker());
        CHECK(got.written_infix() == want->written_infix());
        CHECK(got.sorted_for_lookup() == want->sorted_for_lookup());
        CHECK(got.meta_first() == want->meta_first());
        CHECK(got.tag() == want->tag());
        CHECK(got.values().size() == want->values().size());
        size_t got_meta = 0, want_meta = 0;
        for (const Array& m : got.metadata()) got_meta += m.size() > 0;
        for (const Array& m : want->metadata()) want_meta += m.size() > 0;
        CHECK(got_meta == want_meta);
        // The tag run: each tagged child is found as the copy finds it.
        for (uint32_t i = 0; i < got.size(); ++i) {
            const Symbol t = detail::tag_of(got[i]);
            if (t.empty()) continue;
            const Array* g = got.try_find(t);
            const Array* w = want->try_find(t);
            REQUIRE(g);
            REQUIRE(w);
            CHECK(structurally_equal(*g, *w));
            if (t.is_meta()) CHECK(got.meta(t) == g);
        }
        ++seen;
        return static_cast<int64_t>(got.height());
    }));
    const auto tree = ok(read_string(
        "(size 3) ((deep (er (still 1)))) (@range 0 1) (@doc \"x\") (name \"bob\")", "t.dta"));
    const Node probe(Symbol("probe"));
    const std::span<const Node> all = tree->nodes();
    // Tall, tagged, with metadata after values; then a verb in the method's slot; then flat
    // arguments into the same pooled command, whose height, run and boundary must fall back.
    CHECK(ok(ctx.call(probe, Symbol("go"), all)).try_int() == 5);
    CHECK(ok(ctx.call(probe, Symbol("#remove"), all.subspan(0, 1))).try_int() == 2);
    CHECK(ok(ctx.call(probe, Symbol("go"), all.subspan(2, 3))).try_int() == 2);
    const Node flat[] = {Node(1), Node(2.5), Node(Symbol("s")), Node(1), Node(2)};
    CHECK(ok(ctx.call(probe, Symbol("go"), flat)).try_int() == 1);
    CHECK(ok(ctx.call(probe, Symbol("go"), std::span<const Node>(flat, 3))).try_int() == 1);
    // Past the pool: a fresh command each call, filled alike.
    Node many[12];
    for (Node& n : many) n = all[1];
    CHECK(ok(ctx.call(probe, Symbol("go"), many)).try_int() == 5);
    CHECK(seen == 6);
    // And evaluation reads what was filled: an infix command C++ never wrote stays prefix.
    REQUIRE(ctx.func("count", [](Args a) { return static_cast<int64_t>(a.size()); }));
    CHECK(ok(ctx.call(Node(Symbol("count")), Symbol("x"), flat)).try_int() == 6);
}

TEST_CASE("a pooled command kept by the script is let go, and the next call fills another") {
    Context ctx;
    Mob mob;
    ok(ctx.add(mob, "mob"));
    ok(ctx.eval(mob_class()));
    // {func keep ($a $b) ...} through Context::call, two arguments: the pool's command for
    // two is now the function's definition.
    const auto params = ok(read_string("($x $y)", "p.dta"));
    const Node body = ok(read_string("{+ $x $y}", "p.dta"))->nodes()[0];
    const Node fargs[] = {params->nodes()[0], body};
    REQUIRE(ctx.call(Node(Symbol("func")), Symbol("keep"), fargs));
    CHECK(ok(call_n(ctx, mob, 0)).try_int() == 0);  // {mob w0}: a size the pool has free
    for (int k = 0; k < 3; ++k) CHECK(ok(call_n(ctx, mob, 2)).try_int() == 3);
    CHECK(ok(ctx.eval("{keep 4 5}")).try_int() == 9);
}

TEST_CASE("a thread task with many captures keeps its own sets across ticks; the caller's stay") {
    Context ctx;
    std::vector<std::string> log;
    REQUIRE(ctx.func("log", [&log](Node v) {
        std::string out;
        write(v,
              {[](void* u, std::string_view t) { static_cast<std::string*>(u)->append(t); }, &out});
        log.push_back(out);
    }));
    // A function whose parameter has a capture's name: bound and restored inside a step.
    ok(ctx.eval("{func shadow ($v0) {+ $v0 100}}"));
    // 12 captures: the task sets the even ones each step, reads them all.
    std::string text;
    for (int i = 0; i < 12; ++i)
        text += "{set $v" + std::to_string(i) + " " + std::to_string(i) + "}";
    std::string sum = "{+ 0";
    for (int i = 0; i < 12; ++i) sum += " $v" + std::to_string(i);
    sum += "}";
    std::string step;
    for (int i = 0; i < 12; i += 2)
        step += "{set $v" + std::to_string(i) + " {+ $v" + std::to_string(i) + " 1}}";
    text += "{scheduler thread_task";
    for (int s = 0; s < 4; ++s)
        text += " " + step + " {set $v1 {shadow $v1}} {wait_tick} {log " + sum + "} {log $v0}";
    text += "}";
    ok(ctx.eval(text));
    // The caller changes a captured variable the task does not set, and one it does: the
    // task keeps its own copies of both.
    ok(ctx.eval("{set $v3 1000} {set $v4 2000}"));
    for (int k = 0; k < 6; ++k) ctx.scheduler().tick(1);
    // Step s: the even ones are i + s, $v1 is 1 + 100 s, the rest as captured (sum 66 at 0).
    std::vector<std::string> want;
    for (int s = 1; s <= 4; ++s) {
        want.push_back(std::to_string(66 + 6 * s + 100 * s));
        want.push_back(std::to_string(s));
    }
    CHECK(log == want);
    for (int i = 0; i < 12; ++i) {
        const int64_t caller = i == 3 ? 1000 : i == 4 ? 2000 : i;
        CHECK_MESSAGE(ok(ctx.eval("$v" + std::to_string(i))).try_int() == caller, "$v", i);
    }
}

TEST_CASE("a {wait} condition reads the captures the task's commands set") {
    Context ctx;
    ok(ctx.eval("{set $n 0} {set $m 7}"));
    ok(ctx.eval(
        "{scheduler thread_task {wait {do {set $n {+ $n 1}} {>= $n 3}}} {set $m {+ $m $n}}}"));
    for (int k = 0; k < 6; ++k) ctx.scheduler().tick(1);
    CHECK(ok(ctx.eval("$n")).try_int() == 0);
    CHECK(ok(ctx.eval("$m")).try_int() == 7);
    // The task ran: a global is shared, so it reports there.
    ctx.global("out", Node(0));
    ok(
        ctx.eval("{set $n 0} {scheduler thread_task {wait {do {set $n {+ $n 1}} {>= $n 3}}} "
                 "{set $out $n}}"));
    for (int k = 0; k < 6; ++k) ctx.scheduler().tick(1);
    CHECK(ok(ctx.eval("$out")).try_int() == 3);
}

TEST_CASE("captures among many globals: globals are seen live, the rest captured") {
    Context ctx;
    for (int i = 0; i < 300; ++i)
        (void)ctx.global(Symbol("g" + std::to_string(i)), Node(int64_t{i}));
    // A global named again keeps one entry, with the new doc.
    (void)ctx.global("g7", Node(7), "g7 doc, first");
    (void)ctx.global("g7", Node(7), "g7 doc, second");
    if constexpr (!kShipping) {
        const std::string ref = tools::reference(ctx);
        CHECK(ref.find("g7 doc, second") != std::string::npos);
        CHECK(ref.find("g7 doc, first") == std::string::npos);
    }
    ok(ctx.eval("{set $x 1}"));
    ok(ctx.eval("{scheduler delay_task 1 {set $g299 {+ $g0 $g150 $g299 $x}}}"));
    ok(ctx.eval("{set $x 50} {set $g0 1000} {set $g150 2000}"));
    for (int k = 0; k < 3; ++k) ctx.scheduler().tick(1);
    // $g0 and $g150 were read at the step, $x as captured; $g299's set went to the global.
    CHECK(ok(ctx.eval("$g299")).try_int() == 1000 + 2000 + 299 + 1);
    CHECK(ok(ctx.eval("$x")).try_int() == 50);
}
