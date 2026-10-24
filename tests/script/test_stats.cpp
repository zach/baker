// Call statistics and the trace hook ("Debugging").
#include <doctest.h>
#include <baker/object.hpp>
#include <baker/parse.hpp>
#include <baker/script.hpp>

#include "alloc_counter.hpp"
#include "dev_only.hpp"
#include "ok.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace baker;
using baker::test::ok;

namespace {

// A clock that moves only when {tick n} says so.
uint64_t g_now = 0;
uint64_t fake_now(void*) {
    return g_now;
}

const CallStats* find(const List<CallStats>& all, std::string_view name) {
    for (const CallStats& s : all)
        if (s.name == Symbol(name)) return &s;
    return nullptr;
}

void setup(Context& ctx) {
    REQUIRE(ctx.func("tick", [](int64_t n) { g_now += static_cast<uint64_t>(n); }));
    ok(ctx.eval("{func inner () {tick 7}} {func outer () {tick 5} {inner}}"));
}

}  // namespace

TEST_CASE("calls are counted without profiling, and not timed") {
    Context ctx;
    setup(ctx);
    ok(ctx.eval("{outer} {outer} {inner}"));
    const List<CallStats> s = ctx.stats();
    if constexpr (kShipping) {
        CHECK(s.empty());  // compiled out
        return;
    }
    REQUIRE(find(s, "outer"));
    CHECK(find(s, "outer")->calls == 2);
    CHECK(find(s, "inner")->calls == 3);
    CHECK(find(s, "tick")->calls == 5);
    CHECK(find(s, "tick")->tier == Tier::Native);
    CHECK(find(s, "outer")->tier == Tier::Script);
    CHECK(find(s, "outer")->total_ns == 0);
    CHECK(find(s, "func")->calls == 2);  // built-ins count too: setup defined two functions
}

BAKER_DEV_TEST_CASE("profiling times calls: total includes callees, self does not") {
    Context ctx;
    setup(ctx);
    ctx.reset_stats();
    ctx.set_time_source(&fake_now);
    ctx.set_profiling(true);
    ok(ctx.eval("{outer}"));
    const List<CallStats> s = ctx.stats();
    const CallStats& outer = *find(s, "outer");
    const CallStats& inner = *find(s, "inner");
    const CallStats& tick = *find(s, "tick");
    CHECK(outer.total_ns == 12);
    CHECK(outer.self_ns == 0);
    CHECK(inner.total_ns == 7);
    CHECK(inner.self_ns == 0);
    CHECK(tick.total_ns == 12);
    CHECK(tick.self_ns == 12);
    CHECK(s[0].name == Symbol("tick"));  // ordered by self time
    CHECK(s.size() == 3);

    ctx.reset_stats();
    CHECK(ctx.stats().empty());
}

TEST_CASE("hardening candidates are script functions and handlers over a threshold") {
    if constexpr (kShipping) {
        CHECK(Context().hardening_candidates(0).empty());
        return;
    }
    Context ctx;
    setup(ctx);
    ok(ctx.eval("{class Ball (bounce () {tick 3} {tick 4})}"));
    ctx.reset_stats();
    ctx.set_time_source(&fake_now);
    ctx.set_profiling(true);
    ok(ctx.eval("{outer} {new Ball b} {b bounce}"));
    // Script time here is all in callees, so self times are 0: a threshold of 0 lists
    // every script entry that ran, and none of the natives.
    const List<CallStats> c = ctx.hardening_candidates(0);
    std::vector<std::string> names;
    for (const CallStats& s : c) names.emplace_back(s.name.str());
    CHECK(std::find(names.begin(), names.end(), "tick") == names.end());
    CHECK(std::find(names.begin(), names.end(), "outer") != names.end());
    const CallStats* bounce = find(c, "bounce");
    REQUIRE(bounce);
    CHECK(bounce->on == Symbol("Ball"));
    CHECK(bounce->calls == 1);
    CHECK(bounce->total_ns == 7);
    CHECK(ctx.hardening_candidates(1).empty());
}

TEST_CASE("counting and profiling allocate nothing") {
    if (!test::counting_allocations()) return;
    Context ctx;
    setup(ctx);
    ctx.set_profiling(true);  // the real clock
    ok(ctx.eval("{outer}"));
    const Node call = ok(read_string("{outer}"))->nodes()[0];
    test::AllocationScope scope;
    for (int i = 0; i < 100; ++i) (void)ctx.eval(call);
    CHECK(scope.count() == 0);
}

TEST_CASE("the trace hook sees every call, on entry and on exit") {
    struct Seen {
        std::vector<std::string> lines;
    } seen;
    Context ctx;
    setup(ctx);
    ctx.set_trace_sink(
        [](void* u, const TraceEvent& e) {
            auto& lines = static_cast<Seen*>(u)->lines;
            const std::string head(e.command->try_at(0)->try_sym()->str());
            std::string line = std::string(e.depth * 2, ' ') + head;
            if (e.result) line += *e.result ? " ok" : " error";
            lines.push_back(line);
        },
        &seen);
    ctx.trace(true);
    ok(ctx.eval("{inner}"));
    (void)ctx.eval("{nope}");
    ctx.trace(false);
    ok(ctx.eval("{inner}"));
    if constexpr (kShipping) {
        CHECK(seen.lines.empty());
        return;
    }
    CHECK(seen.lines == std::vector<std::string>{"inner", "  tick", "  tick ok", "inner ok", "nope",
                                                 "nope error"});
}
