// Typed messages: the C++ path, the script path, and where they meet.
#include <doctest.h>
#include <baker/object.hpp>
#include <baker/script.hpp>
#include <baker/std_script.hpp>

#include "alloc_counter.hpp"
#include "ok.hpp"

#include <string>
#include <vector>

using namespace baker;
using baker::test::ok;

namespace {

struct HitMsg {
    static constexpr auto name = "hit";
    int points;
};
struct MoveMsg {
    static constexpr auto name = "move";
    float x;
    float y;
    Symbol how;
};
struct PingMsg {
    static constexpr auto name = "ping";
};
struct WaveMsg {  // bound only as a plain method
    static constexpr auto name = "wave";
    int times;
};

static_assert(detail::field_count<HitMsg>() == 1);
static_assert(detail::field_count<MoveMsg>() == 3);
static_assert(detail::field_count<PingMsg>() == 0);
static_assert(std::is_same_v<detail::FieldTypes<MoveMsg>, detail::TypeList<float, float, Symbol>>);

struct Unit : Object<Unit> {
    int hp = 100;
    float x = 0, y = 0;
    Symbol last;
    int pings = 0;

    int on_hit(const HitMsg& m) { return hp -= m.points; }
    void on_move(const MoveMsg& m) {
        x = m.x;
        y = m.y;
        last = m.how;
    }
    static void bind(Binder<Unit>& b) {
        b.on<HitMsg>(&Unit::on_hit);
        b.on<MoveMsg>(&Unit::on_move);
        b.on<PingMsg>([](Unit& u, const PingMsg&) { ++u.pings; });
        b.method("wave", [](Unit&, int times) { return times * 2; });
        b.property("hp", &Unit::hp);
    }
};

// Inherits Unit's handlers, and replaces one.
struct Tank : Object<Tank, Unit> {
    static void bind(Binder<Tank>& b) {
        b.on<HitMsg>([](Tank& t, const HitMsg& m) { return t.hp -= m.points / 2; });
    }
};

struct Rock : Object<Rock> {};

}  // namespace

TEST_CASE("C++ to C++: a typed message reaches its handler with no Context") {
    Unit u;
    CHECK(ok(u.handle(HitMsg{.points = 20})).try_int() == 80);
    CHECK(ok(u.handle(MoveMsg{1.5f, -2.0f, Symbol("run")})).try_int() == 0);  // void: Int 0
    CHECK(u.x == 1.5f);
    CHECK(u.last == Symbol("run"));
    CHECK(ok(u.handle(PingMsg{})).try_int() == 0);
    CHECK(u.pings == 1);
    CHECK(u.handles<HitMsg>());
    CHECK_FALSE(u.handles<WaveMsg>());  // a plain method needs a Context to be called
}

TEST_CASE("the typed path allocates nothing, in a Context or not") {
    if (!test::counting_allocations()) return;
    Unit loose, placed;
    Context ctx;
    ok(ctx.add(placed, "u1"));
    (void)loose.handle(HitMsg{1});
    (void)placed.handle(HitMsg{1});
    test::AllocationScope scope;
    for (int i = 0; i < 100; ++i) {
        (void)loose.handle(HitMsg{1});
        (void)placed.handle(HitMsg{1});
        (void)placed.handles<MoveMsg>();
    }
    CHECK(scope.count() == 0);
}

TEST_CASE("script to C++: {u1 hit 20} reaches on<HitMsg>, fields in order") {
    Context ctx;
    Unit u;
    ok(ctx.add(u, "u1"));
    CHECK(ok(ctx.eval("{u1 hit 20}")).try_int() == 80);
    ok(ctx.eval("{u1 move 3 4.5 walk}"));
    CHECK(u.x == 3.0f);
    CHECK(u.y == 4.5f);
    CHECK(u.last == Symbol("walk"));
    const auto bad = ctx.eval("{u1 hit}", "m.dta");
    REQUIRE_FALSE(bad);
    CHECK(std::string(bad.error().message()) == "'hit' takes 1 argument, given 0");
}

TEST_CASE("C++ to script: a script handler for the message's name comes first") {
    Context ctx;
    Unit u, other;
    ok(ctx.add(u, "u1"));
    ok(ctx.eval("{class Unit (hit ($points) {set [hp] {- [hp] {* $points 2}}} -1)}"));
    CHECK(ok(u.handle(HitMsg{10})).try_int() == -1);          // the script's result
    CHECK(u.hp == 80);                                        // and the script's arithmetic
    CHECK(other.handle(HitMsg{10}).value().try_int() == 90);  // in no Context: typed path
    CHECK(u.handles<HitMsg>());
}

TEST_CASE("a message bound only as a method is reached through the Context") {
    Context ctx;
    Unit u;
    ok(ctx.add(u, "u1"));
    CHECK(u.handles<WaveMsg>());
    CHECK(ok(u.handle(WaveMsg{3})).try_int() == 6);
}

TEST_CASE("an unhandled message is a NotFound error naming the type and message") {
    Rock r;
    CHECK_FALSE(r.handles<HitMsg>());
    const auto e = r.handle(HitMsg{1});
    REQUIRE_FALSE(e);
    CHECK(e.error().code() == ErrorCode::NotFound);
    CHECK(std::string(e.error().message()) == "Rock does not handle the message 'hit'");
}

TEST_CASE("handlers are inherited; a derived handler replaces its base's") {
    Tank t;
    CHECK(ok(t.handle(HitMsg{20})).try_int() == 90);
    ok(t.handle(MoveMsg{7, 8, Symbol("roll")}));  // inherited from Unit
    CHECK(t.x == 7.0f);
    Context ctx;
    ok(ctx.add(t, "t1"));
    CHECK(ok(ctx.eval("{t1 hit 20}")).try_int() == 80);  // the script face is replaced too
}

TEST_CASE("handle_all sends to many, skipping objects that do not handle it") {
    Unit a, b;
    Tank t;
    Rock r;
    ObjectBase* all[] = {&a, &b, &t, &r};
    CHECK(ok(handle_all(std::span<ObjectBase* const>(all), HitMsg{10})) == 3);
    CHECK(a.hp == 90);
    CHECK(b.hp == 90);
    CHECK(t.hp == 95);
    if (!test::counting_allocations()) return;
    test::AllocationScope scope;
    (void)handle_all(std::span<ObjectBase* const>(all), HitMsg{1});
    CHECK(scope.count() == 0);
}

TEST_CASE("handle_all reaches script handlers and stops at the first error") {
    Context ctx;
    Unit a, b;
    ok(ctx.add(a, "a"));
    ok(ctx.add(b, "b"));
    ok(ctx.eval("{class Unit (hit ($p) {if {> $p 50} {nope} 0})}"));
    ObjectBase* both[] = {&a, &b};
    CHECK(ok(handle_all(std::span<ObjectBase* const>(both), HitMsg{10})) == 2);
    const auto r = handle_all(std::span<ObjectBase* const>(both), HitMsg{60});
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::NotFound);  // {nope} is not a function
}

TEST_CASE("the reference marks a message's method") {
    Context ctx;
    ctx.add_type<Unit>();
    const std::string ref = reference(ctx);
    CHECK(ref.find("- `{<Unit> hit int32}` → int32 (message `HitMsg`)\n") != std::string::npos);
    CHECK(ref.find("- `{<Unit> move float32 float32 symbol}` → nothing (message `MoveMsg`)\n") !=
          std::string::npos);
}
