// Script tasks: {scheduler delay_task|interp_task|thread_task ...}.
#include <doctest.h>
#include <baker/object.hpp>
#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/task.hpp>
#include <baker/write.hpp>

#include "alloc_counter.hpp"
#include "ok.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace baker;
using baker::test::ok;

namespace {

// A Context whose {log v} appends v's text, and whose notices are kept.
struct Fixture {
    Context ctx;
    std::vector<std::string> log;
    std::vector<std::string> notices;
    Fixture() {
        REQUIRE(ctx.func("log", [this](Node v) {
            std::string out;
            write(v, {[](void* u, std::string_view t) { static_cast<std::string*>(u)->append(t); },
                      &out});
            log.push_back(out);
        }));
        ctx.set_notice_sink(
            [](void* u, const Error& e) {
                static_cast<Fixture*>(u)->notices.emplace_back(describe(e));
            },
            this);
    }
    void ticks(int n, double dt = 1) {
        for (int i = 0; i < n; ++i) ctx.scheduler().tick(dt);
    }
};

}  // namespace

TEST_CASE("delay_task runs once, later, with the variables it named captured by value") {
    Fixture f;
    ok(f.ctx.eval("{do (($n 5)) {scheduler delay_task 2 {log $n}}}"));  // $n is gone after {do}
    f.ticks(2);
    CHECK(f.log.empty());  // starts at tick 1, due at its time + 2
    f.ticks(1);
    CHECK(f.log == std::vector<std::string>{"5"});
    f.ticks(5);
    CHECK(f.log.size() == 1);
}

TEST_CASE("a task's own {set}s persist in its copy; the caller's variable is untouched") {
    Fixture f;
    ok(
        f.ctx.eval("{set $i 0} {scheduler thread_task {set $i {+ $i 1}} {wait_tick} {log $i} "
                   "{set $i {+ $i 10}} {wait_tick} {log $i}}"));
    f.ticks(3);
    CHECK(f.log == std::vector<std::string>{"1", "11"});
    CHECK(ok(f.ctx.eval("$i")).try_int() == 0);
}

TEST_CASE("globals are not captured: a task sees their current value") {
    Fixture f;
    f.ctx.global("level", Node(1));
    ok(f.ctx.eval("{scheduler delay_task 1 {log $level}}"));
    ok(f.ctx.eval("{set $level 2}"));
    f.ticks(3);
    CHECK(f.log == std::vector<std::string>{"2"});
}

TEST_CASE("interp_task steps from `from` to `to` over |to - from| units, ending exactly at `to`") {
    Fixture f;
    ok(f.ctx.eval("{scheduler interp_task $frame 0 4 {log $frame}}"));
    f.ticks(8);
    CHECK(f.log == std::vector<std::string>{"0.0", "1.0", "2.0", "3.0", "4.0"});
    f.log.clear();
    ok(f.ctx.eval("{scheduler interp_task $x 3 0 {log $x}}"));
    f.ticks(6, 1.5);
    CHECK(f.log == std::vector<std::string>{"3.0", "1.5", "0.0"});
}

TEST_CASE("thread_task waits on a condition, a time, and a tick, as its own commands") {
    Fixture f;
    f.ctx.global("ready", Node(0));
    ok(
        f.ctx.eval("{scheduler thread_task {log a} {wait $ready} {log b} {wait_for 2} {log c} "
                   "{wait_tick} {log d}}"));
    f.ticks(3);
    CHECK(f.log == std::vector<std::string>{"a"});
    ok(f.ctx.eval("{set $ready 1}"));
    f.ticks(1);
    CHECK(f.log == std::vector<std::string>{"a", "b"});
    f.ticks(2);
    CHECK(f.log == std::vector<std::string>{"a", "b", "c"});
    f.ticks(1);
    CHECK(f.log == std::vector<std::string>{"a", "b", "c", "d"});
}

TEST_CASE("{wait} anywhere but a thread_task's own commands is an error") {
    Fixture f;
    const auto r = f.ctx.eval("{if 1 {wait 1}}");
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::Refused);
    CHECK(std::string(r.error().message()) ==
          "{wait} works only as a command of {scheduler thread_task ...}");
}

TEST_CASE("a task made in a handler ends when its $this object is removed") {
    Fixture f;
    ok(
        f.ctx.eval("{class Ball (go () {scheduler thread_task {log 1} {wait_tick} {log 2}})} "
                   "{new Ball b} {b go}"));
    f.ticks(1);
    CHECK(f.log == std::vector<std::string>{"1"});
    ok(f.ctx.eval("{delete b}"));
    f.ticks(3);
    CHECK(f.log == std::vector<std::string>{"1"});
}

TEST_CASE("cancel ends a task by the handle its command returned") {
    Fixture f;
    ok(f.ctx.eval("{set $h {scheduler delay_task 1 {log late}}} {scheduler cancel $h}"));
    f.ticks(3);
    CHECK(f.log.empty());
}

TEST_CASE("an error ends the task and goes to the notice sink, with the task on its stack") {
    Fixture f;
    ok(f.ctx.eval("{scheduler thread_task {log a} {nope} {log b}}", "tasks.dta"));
    f.ticks(2);
    CHECK(f.log == std::vector<std::string>{"a"});
    REQUIRE(f.notices.size() == 1);
    CHECK(f.notices[0].find("'nope' is not a function") != std::string::npos);
    CHECK(f.notices[0].find("in {thread_task}  (tasks.dta:1)") != std::string::npos);
}

TEST_CASE("a task captures at most 16 variables") {
    Fixture f;
    std::string vars, uses;
    for (int i = 0; i < 17; ++i) {
        vars += "($v" + std::to_string(i) + " " + std::to_string(i) + ")";
        uses += " $v" + std::to_string(i);
    }
    const auto r = f.ctx.eval("{do (" + vars + ") {scheduler delay_task 1 {log" + uses + "}}}");
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::Limit);
}

TEST_CASE("making and running a script task allocates nothing once warm") {
    if (!test::counting_allocations()) return;
    Fixture f;
    REQUIRE(f.ctx.func("tally", [](int64_t) {}));
    auto tree =
        ok(read_string("{do (($n 1)) {scheduler thread_task {tally $n} {wait_tick} "
                       "{tally $n}}}"));
    const Node make = (*tree)[0];
    for (int i = 0; i < 70; ++i) ok(f.ctx.eval(make));
    f.ticks(3);
    test::AllocationScope scope;
    for (int i = 0; i < 64; ++i) (void)f.ctx.eval(make);
    f.ticks(3);
    CHECK(scope.count() == 0);
}

TEST_CASE("a {delete} during a tick waits for its end: later tasks in the batch still see it") {
    Fixture f;
    ok(
        f.ctx.eval("{class Ball} {new Ball b} "
                   "{scheduler thread_task {delete b} {log {b type}}} "
                   "{scheduler thread_task {log {b type}}}"));
    f.ticks(1);
    CHECK(f.log == std::vector<std::string>{"Ball", "Ball"});
    const auto gone = f.ctx.eval("{b type}");
    REQUIRE_FALSE(gone);  // removed at the sync point
    CHECK(gone.error().code() == ErrorCode::NotFound);
}

TEST_CASE("remove() from a C++ task is deferred too; ctx.sync() applies it") {
    struct Thing : Object<Thing> {};
    Context ctx;
    Thing t;
    ok(ctx.add(t, "thing"));
    bool seen_during = false;
    auto remover = [](Scheduler&, Context& c, Thing& th, bool* seen) -> Task {
        c.remove(th);
        *seen = c.object("thing").type() == Type::Object;
        co_return;
    };
    ok(ctx.scheduler().spawn(remover(ctx.scheduler(), ctx, t, &seen_during)));
    ctx.scheduler().tick(1);
    CHECK(seen_during);
    CHECK(ctx.object("thing").type() == Type::Int);  // gone after the tick
    CHECK(t.context() == nullptr);
    ok(ctx.add(t, "thing"));
    ctx.remove(t);  // outside a tick: at once
    CHECK(t.context() == nullptr);
    ctx.sync();  // nothing waiting: harmless
}

TEST_CASE("a C++ object destroyed during a tick leaves its Context at once") {
    struct Thing : Object<Thing> {};
    Context ctx;
    auto t = std::make_unique<Thing>();
    ok(ctx.add(*t, "thing"));
    ok(ctx.eval("{thing set mood happy}"));
    auto destroyer = [](Scheduler&, Context& c, std::unique_ptr<Thing>* owned, bool* gone) -> Task {
        owned->reset();  // the destructor runs mid-tick
        *gone = c.object("thing").type() != Type::Object;
        co_return;
    };
    bool gone_during = false;
    ok(ctx.scheduler().spawn(destroyer(ctx.scheduler(), ctx, &t, &gone_during)));
    ctx.scheduler().tick(1);  // the sync point finds nothing left to touch (ASan checks)
    CHECK(gone_during);
    CHECK(ctx.object("thing").type() != Type::Object);

    // Removed during a tick, then destroyed before its end: the deferred removal finds the
    // slot already gone.
    auto u = std::make_unique<Thing>();
    ok(ctx.add(*u, "other"));
    auto both = [](Scheduler&, Context& c, std::unique_ptr<Thing>* owned) -> Task {
        c.remove(**owned);
        owned->reset();
        co_return;
    };
    ok(ctx.scheduler().spawn(both(ctx.scheduler(), ctx, &u)));
    ctx.scheduler().tick(1);
    CHECK(ctx.object("other").type() != Type::Object);
}
