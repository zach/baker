// Tasks: C++ coroutines on a Scheduler.
#include <doctest.h>
#include <baker/script.hpp>
#include <baker/task.hpp>

#include "alloc_counter.hpp"
#include "ok.hpp"

#include <string>
#include <vector>

using namespace baker;
using baker::test::ok;

namespace {

using Log = std::vector<std::string>;

void note(Log& log, const Scheduler& s, const std::string& what) {
    log.push_back(what + "@" + std::to_string(static_cast<int>(s.now() * 10 + 0.5)));
}

Task blinker(Scheduler& s, Log& log) {
    for (int i = 0; i < 2; ++i) {
        note(log, s, "on" + std::to_string(i));  // `i` lives across every suspend
        co_await delay(0.3);
        note(log, s, "off" + std::to_string(i));
        co_await next_tick();
    }
    note(log, s, "done");
}

Task waiter(Context& ctx, Log& log, const bool& flag) {  // a Context: its default Scheduler
    co_await until([&] { return flag; });
    note(log, ctx.scheduler(), "flag");
}

Task one_tick(Scheduler&) {
    co_await next_tick();
}
// Its predicate spawns enough tasks to move the Scheduler's slots.
Task spawner(Scheduler& s, int* polls) {
    co_await until([&s, polls] {
        if (++*polls == 1) return false;
        for (int i = 0; i < 200; ++i) (void)s.spawn(one_tick(s));
        return true;
    });
}

struct Guard {
    int* destroyed;
    ~Guard() { ++*destroyed; }
};
Task forever(Scheduler&, int* destroyed) {  // the Scheduler only picks the pool
    Guard g{destroyed};
    for (;;) co_await delay(1);
}

// Its array is written before a suspend and read after, so it must live in the frame.
Task big(Scheduler&, int* sum) {
    char bytes[8192];
    for (size_t i = 0; i < sizeof bytes; ++i) bytes[i] = static_cast<char>(i * 7);
    co_await next_tick();
    for (char b : bytes) *sum += b;
}

struct Light {
    int on = 0;
    Task flicker(Scheduler&, int times) {  // a member: the object comes first, the Scheduler later
        for (int i = 0; i < times; ++i) {
            ++on;
            co_await next_tick();
        }
    }
};

}  // namespace

TEST_CASE("delay and next_tick resume at the right ticks, keeping locals across suspends") {
    Context ctx;
    Scheduler& s = ctx.scheduler();
    Log log;
    ok(s.spawn(blinker(s, log), "blink"));
    CHECK(log.empty());  // a task starts at the next tick
    for (int i = 0; i < 12; ++i) s.tick(0.1);
    CHECK(log == Log{"on0@1", "off0@4", "on1@5", "off1@8", "done@9"});
    CHECK(s.stats().live == 0);
}

TEST_CASE("until polls each tick; a Context& parameter means the default Scheduler") {
    Context ctx;
    Log log;
    bool flag = false;
    ok(ctx.scheduler().spawn(waiter(ctx, log, flag)));
    ctx.scheduler().tick(1);
    ctx.scheduler().tick(1);
    CHECK(log.empty());
    flag = true;
    ctx.scheduler().tick(1);
    CHECK(log == Log{"flag@30"});
}

TEST_CASE("tasks due in one tick run by wake time, then spawn order") {
    Context ctx;
    Scheduler& s = ctx.scheduler();
    Log log;
    auto at = [](Scheduler&, Log& l, double t, std::string what) -> Task {
        co_await delay(t);
        l.push_back(what);
    };
    ok(s.spawn(at(s, log, 0.5, "b")));
    ok(s.spawn(at(s, log, 0.2, "a")));
    ok(s.spawn(at(s, log, 0.5, "c")));
    s.tick(0);  // start them
    s.tick(1);  // all due in one tick
    CHECK(log == Log{"a", "b", "c"});
}

TEST_CASE("a member coroutine finds its Scheduler among later parameters") {
    Context ctx;
    Light light;
    ok(ctx.scheduler().spawn(light.flicker(ctx.scheduler(), 3)));
    for (int i = 0; i < 5; ++i) ctx.scheduler().tick(1);
    CHECK(light.on == 3);
}

TEST_CASE("cancel destroys the frame, at once outside a tick and at its end inside one") {
    Context ctx;
    Scheduler& s = ctx.scheduler();
    int destroyed = 0;
    const TaskHandle a = ok(s.spawn(forever(s, &destroyed)));
    s.tick(1);
    CHECK(s.running(a));
    s.cancel(a);
    CHECK(destroyed == 1);  // outside a tick: at once
    CHECK_FALSE(s.running(a));
    s.cancel(a);  // a stale handle is harmless

    const TaskHandle b = ok(s.spawn(forever(s, &destroyed)));
    bool seen_running = false;
    auto killer = [](Scheduler& sc, TaskHandle victim, int* d, bool* seen) -> Task {
        sc.cancel(victim);
        *seen = *d == 1;  // not destroyed yet: the cancel waits for the sync point
        co_return;
    };
    ok(s.spawn(killer(s, b, &destroyed, &seen_running)));
    s.tick(1);
    CHECK(seen_running);
    CHECK(destroyed == 2);
    CHECK(s.stats().live == 0);
}

TEST_CASE("a frame over the budget fails at spawn, naming the task and both sizes") {
    Context ctx;
    Scheduler& s = ctx.scheduler();
    int sum = 0;
    const auto r = s.spawn(big(s, &sum), "huge");
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::Limit);
    const std::string msg(r.error().message());
    CHECK(msg.find("task 'huge' needs a frame of ") == 0);
    CHECK(msg.find("the Scheduler's budget is 4096") != std::string::npos);
    Scheduler roomy(ctx, {.frame_bytes = 16384});
    ok(roomy.spawn(big(roomy, &sum), "huge"));
    roomy.tick(0);
    roomy.tick(0);
    CHECK(sum != 0);
}

TEST_CASE("max_tasks caps live tasks; a task made for another Scheduler is refused") {
    Context ctx;
    Scheduler capped(ctx, {.max_tasks = 1});
    int destroyed = 0;
    ok(capped.spawn(forever(capped, &destroyed)));
    CHECK(capped.spawn(forever(capped, &destroyed)).error().code() == ErrorCode::Limit);
    CHECK(destroyed == 0);  // the refused task never started, so its locals never existed
    CHECK(capped.stats().live == 1);
    Scheduler other(ctx);
    CHECK(capped.spawn(forever(other, &destroyed)).error().code() == ErrorCode::Refused);
}

TEST_CASE("destroying a Scheduler destroys its tasks' frames") {
    Context ctx;
    int destroyed = 0;
    {
        Scheduler s(ctx);
        ok(s.spawn(forever(s, &destroyed)));
        ok(s.spawn(forever(s, &destroyed)));
        s.tick(1);
    }
    CHECK(destroyed == 2);
}

TEST_CASE("spawning and stepping allocate nothing once the pool is warm") {
    if (!test::counting_allocations()) return;
    Context ctx;
    Scheduler& s = ctx.scheduler();
    Log log;
    auto step = [](Scheduler&, int* n) -> Task {
        for (int i = 0; i < 3; ++i) {
            ++*n;
            co_await next_tick();
        }
    };
    int n = 0;
    for (int i = 0; i < 64; ++i) ok(s.spawn(step(s, &n), "stepper"));
    for (int i = 0; i < 5; ++i) s.tick(1);
    REQUIRE(n == 192);
    test::AllocationScope scope;
    for (int i = 0; i < 64; ++i) (void)s.spawn(step(s, &n), "stepper");  // named: no string built
    for (int i = 0; i < 5; ++i) s.tick(1);
    CHECK(scope.count() == 0);
    CHECK(n == 384);
    const SchedulerStats st = s.stats();
    CHECK(st.peak == 64);
    CHECK(st.blocks == 64);
    MESSAGE("a small task's frame: " << st.largest_frame << " bytes");
}

TEST_CASE("an until predicate may spawn tasks") {
    Context ctx;
    Scheduler& s = ctx.scheduler();
    int polls = 0;
    REQUIRE(s.spawn(spawner(s, &polls)));
    for (int i = 0; i < 4; ++i) s.tick(1);
    CHECK(polls == 2);
    CHECK(s.stats().live == 0);
}
