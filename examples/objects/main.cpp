// Objects: a C++ type callable from script, a typed message, a script handler attached to
// the type, and a reference that outlives its object.
//
//   baker_example_objects
#include <baker/object.hpp>
#include <baker/std_script.hpp>

#include <cstdio>

// A message: a plain aggregate with a script name. Its fields are its parameters.
struct HitMsg {
    static constexpr auto name = "hit";
    int points;
};

struct Unit : baker::Object<Unit> {
    int hp = 100;
    float x = 0;

    int on_hit(const HitMsg& m) { return hp -= m.points; }
    float dash(float distance) { return x += distance; }

    static void bind(baker::Binder<Unit>& b) {
        b.method("dash", &Unit::dash).doc("Moves `distance` along x.");
        b.property("hp", &Unit::hp);
        b.on<HitMsg>(&Unit::on_hit);  // also the method {unit hit points}
    }
};

// Inherits Unit's methods, properties, and message handlers.
struct Boss : baker::Object<Boss, Unit> {
    static void bind(baker::Binder<Boss>&) {}
};

namespace {

baker::Context ctx;

void show(const char* text) {
    auto r = ctx.eval(text);
    std::printf("%-26s => %s\n", text,
                r ? baker::to_string(*r).c_str() : baker::to_string(r.error()).data());
}

}  // namespace

int main() {
    Unit grunt;
    Boss boss;
    boss.hp = 500;
    if (!ctx.add(grunt, "grunt") || !ctx.add(boss, "boss")) return 1;

    show("{grunt dash 3}");  // script calls a C++ method
    show("{grunt get hp}");  // and reads a property
    // C++ to C++: no Node is built and nothing allocates.
    std::printf("grunt.handle(HitMsg{20})   => %lld\n",
                static_cast<long long>(grunt.handle(HitMsg{20})->try_int().value_or(-1)));
    show("{grunt hit 5}");  // the same handler, from script

    // A designer gives bosses armor in script, without touching the C++. Script handlers
    // come first, for script and C++ senders alike.
    show("{class Boss (hit ($points) {set [hp] {- [hp] 1}} [hp])}");
    std::printf("boss.handle(HitMsg{50})    => %lld\n",
                static_cast<long long>(boss.handle(HitMsg{50})->try_int().value_or(-1)));
    show("{grunt hit 50}");  // grunts are unarmored

    // Script data on a C++ object, and C++ calling a script handler.
    show("{class Unit (taunt {set [line] \"Over here!\"} [line])}");
    if (auto r = ctx.call(grunt, "taunt"))
        std::printf("ctx.call(grunt, \"taunt\")   => %s\n", baker::to_string(*r).c_str());

    // A reference kept after its object is destroyed is a clean error, not a crash.
    {
        Unit temp;
        auto node = ctx.add(temp);
        if (!node) return 1;
        ctx.var("temp") = *node;
    }
    show("{$temp dash 1}");
    return 0;
}
