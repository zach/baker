// Native call cost. Target: a native function call through the binding layer
// with 2 scalar args <= 40 ns, with zero allocations.
//
// Each row evaluates a parsed command node with Context::eval, as a game evaluating
// its data would. The last rows compare the per-Context table that resolves a function
// name with a hash map, the lookup an in-node cache would save.
//
// Cold rows: 20,000 native functions, and 2,000 named objects, each called in a
// shuffled order, so the commands, function records, and objects are not in cache. Gaps 2m
// and 6: 2,048 objects of 64 types, with 32 methods or one, called by script and sent a
// message from C++, so the method tables and the types' vtables are not in cache either.
#include <baker/object.hpp>
#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/std_interop.hpp>

#include "alloc_counter.hpp"
#include "bench_call_types.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

volatile double g_sink;

template <class T>
const T& opaque(const T& v) {
    const T* volatile p = &v;
    return *p;
}

template <class F>
double ns_per_call(F&& f) {
    constexpr int kCalls = 2'000'000;
    double best = 1e30;
    for (int rep = 0; rep < 7; ++rep) {
        double sum = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kCalls; ++i) sum += f(i);
        const auto t1 = std::chrono::steady_clock::now();
        g_sink = sum;
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / kCalls);
    }
    return best;
}

}  // namespace

struct Person : baker::Object<Person> {
    float height = 6.0f;
    int hp = 1'000'000;
    float on_grow(float amount) { return height += amount; }
    int on_hit(const HitMsg& m) { return hp -= m.points; }
    static void bind(baker::Binder<Person>& b) {
        b.method("grow", &Person::on_grow);
        b.on<HitMsg>(&Person::on_hit);
    }
};

// A type whose HitMsg is handled by a script class, not by C++.
struct Scripted : baker::Object<Scripted> {
    static void bind(baker::Binder<Scripted>&) {}
};

constexpr int kPerType = 32;

// kPerType objects of each type, interleaved by type as type 0, 1, ..., 63, 0, 1, ...
template <int... P>
void make_objects(int methods, Objects& out, std::integer_sequence<int, P...>) {
    for (int i = 0; i < kPerType; ++i) (push_part<P>(methods, out), ...);
}

int main() {
    baker::Context ctx;
    Person bob;
    if (!ctx.add(bob, "bob")) return 1;
    if (!ctx.func("add", [](int64_t a, int64_t b) { return a + b; })) return 1;
    ctx.var("x") = 4;
    // Array-valued variables: reading one copies the node, which counts the array's owner.
    auto loaded = baker::read_string("(1 2 3)", "list.dta");
    if (!loaded) return 1;
    ctx.var("loaded") = (**loaded)[0];
    ctx.var("solo") = baker::Node(baker::Array::make(3));
    if (!ctx.eval("{func add1 ($a) {+ $a 1}}")) return 1;
    if (!ctx.func("len", [](const baker::Array& a) { return static_cast<int64_t>(a.size()); }))
        return 1;
    auto tree = baker::read_string(
        "{add 1 2} {+ 1 2} {+ 1.5 2} {add $x 1} $x {if 1 2 3} {add1 1} {add {add 1 2} 3} "
        "{bob grow 0} $loaded $solo {len (1 2 3)}",
        "bench.dta");
    if (!tree) return 1;
    const char* names[] = {
        "{add 1 2}: native, 2 int args",
        "{+ 1 2}: overload set (int int)",
        "{+ 1.5 2}: overload set, promoted",
        "{add $x 1}: a variable argument",
        "$x: a variable",
        "{if 1 2 3}: lazy branches",
        "{add1 1}: script function",
        "{add {add 1 2} 3}: nested",
        "{bob grow 0}: native method",
        "$loaded: an array from a load",
        "$solo: an array built by the host",
        "{len (1 2 3)}: a const Array& argument",
    };

    std::printf("evaluating a command node (best of 7, 2M calls each)\n");
    for (size_t k = 0; k < (*tree)->size(); ++k) {
        const baker::Node& node = (**tree)[k];
        auto call = [&](int) {
            auto r = ctx.eval(opaque(node));
            return r ? static_cast<double>(r->try_float().value_or(0)) : -1.0;
        };
        for (int i = 0; i < 100; ++i) call(i);
        baker::test::AllocationScope scope;
        for (int i = 0; i < 1000; ++i) call(i);
        const size_t allocs = scope.count();
        const double ns = ns_per_call(call);
        if (k == 0)
            std::printf("%-38s %7.2f ns  %zu allocations / 1000 calls   (target <= 40 ns, 0: %s)\n",
                        names[k], ns, allocs, ns <= 40 && allocs == 0 ? "ok" : "MISSED");
        else
            std::printf("%-38s %7.2f ns  %zu allocations / 1000 calls\n", names[k], ns, allocs);
    }

    // Call statistics and tracing: counting is always on outside shipping builds;
    // timing and tracing are switched on here.
    std::printf("\n{add 1 2} with the development aids switched on\n");
    {
        const baker::Node& add = (**tree)[0];
        auto call = [&](int) {
            auto r = ctx.eval(opaque(add));
            return r ? static_cast<double>(r->try_float().value_or(0)) : -1.0;
        };
        ctx.set_profiling(true);
        std::printf("%-38s %7.2f ns\n", "profiling (two steady_clock reads)", ns_per_call(call));
        ctx.set_profiling(false);
        ctx.set_trace_sink([](void*, const baker::TraceEvent&) {}, nullptr);
        ctx.trace(true);
        std::printf("%-38s %7.2f ns\n", "tracing to an empty sink", ns_per_call(call));
        ctx.trace(false);
    }

    // Typed messages: C++ to C++ dispatch in 15 ns or less, with no allocation.
    std::printf("\nsending a message from C++\n");
    {
        auto row = [](const char* name, auto&& call, const char* target) {
            for (int i = 0; i < 100; ++i) call(i);
            baker::test::AllocationScope scope;
            for (int i = 0; i < 1000; ++i) call(i);
            const size_t allocs = scope.count();
            const double ns = ns_per_call(call);
            std::printf("%-38s %7.2f ns  %zu allocations / 1000 calls%s\n", name, ns, allocs,
                        target);
        };
        Person loose;
        Person* volatile to_bob = &bob;  // opaque, and not const
        Person* volatile to_loose = &loose;
        row(
            "bob.handle(HitMsg{1}), in a Context",
            [&](int i) {
                auto r = to_bob->handle(HitMsg{i & 1});
                return r ? static_cast<double>(r->try_int().value_or(0)) : -1.0;
            },
            "   (target <= 15 ns)");
        row(
            "handle(HitMsg{1}), in no Context",
            [&](int i) {
                auto r = to_loose->handle(HitMsg{i & 1});
                return r ? static_cast<double>(r->try_int().value_or(0)) : -1.0;
            },
            "");
        std::vector<Person> crowd(64);
        std::vector<baker::ObjectBase*> ptrs;
        for (Person& p : crowd) ptrs.push_back(&p);
        row(
            "handle_all, one call for 64 objects",
            [&](int i) {
                auto r =
                    baker::handle_all(std::span<baker::ObjectBase* const>(ptrs), HitMsg{i & 1});
                return r ? static_cast<double>(*r) : -1.0;
            },
            "   (the time is for 64)");
        // Script handlers reached from C++, and a native method once a {class} exists.
        Scripted scripted;
        if (!ctx.add(scripted, "scripted") || !ctx.eval("{class Scripted (hit ($points) $points)}"))
            std::abort();
        Scripted* volatile to_scripted = &scripted;
        row(
            "handle(HitMsg{1}), to a script handler",
            [&](int i) {
                auto r = to_scripted->handle(HitMsg{i & 1});
                return r ? static_cast<double>(r->try_int().value_or(0)) : -1.0;
            },
            "");
        const baker::Node grow = (**tree)[8];  // {bob grow 0}
        row(
            "{bob grow 0}, a {class} for another type",
            [&](int) {
                auto r = ctx.eval(opaque(grow));
                return r ? r->try_float().value_or(0) : -1.0;
            },
            "");
        row(
            "ctx.call(bob, \"grow\", 1.5f), for comparison",
            [&](int) {
                auto r = ctx.call(opaque(bob), "grow", 1.5f);
                return r ? r->try_float().value_or(0) : -1.0;
            },
            "");
    }

    // Cold: many functions and objects, called in a shuffled order.
    std::printf("\ncold: calls in a shuffled order (best of 7, 2M calls each)\n");
    {
        constexpr int kFunctions = 20'000, kObjects = 2'000;
        std::string text;
        for (int i = 0; i < kFunctions; ++i) {
            const std::string name = "f" + std::to_string(i);
            if (!ctx.func(baker::Symbol(name), [](int64_t a, int64_t b) { return a + b; }))
                return 1;
            text += "{" + name + " 1 2} ";
        }
        std::vector<Person> people(kObjects);
        for (int i = 0; i < kObjects; ++i) {
            if (!ctx.add(people[static_cast<size_t>(i)], baker::Symbol("p" + std::to_string(i))))
                return 1;
            text += "{p" + std::to_string(i) + " grow 0} ";
        }
        auto cold = baker::read_string(text, "cold.dta");
        if (!cold) return 1;
        std::mt19937 rng(42);
        std::vector<uint32_t> order(kFunctions), objects(kObjects);
        for (uint32_t i = 0; i < kFunctions; ++i) order[i] = i;
        for (uint32_t i = 0; i < kObjects; ++i) objects[i] = kFunctions + i;
        std::shuffle(order.begin(), order.end(), rng);
        std::shuffle(objects.begin(), objects.end(), rng);
        auto run = [&](const std::vector<uint32_t>& which) {
            return ns_per_call([&](int i) {
                const auto u = static_cast<unsigned>(i);
                auto r = ctx.eval((**cold)[which[u % which.size()]]);
                return r ? static_cast<double>(r->try_float().value_or(0)) : -1.0;
            });
        };
        std::printf("%-38s %7.2f ns\n", "{fN 1 2}: one of 20,000 natives", run(order));
        std::printf("%-38s %7.2f ns\n", "{pN grow 0}: one of 2,000 objects", run(objects));
        for (Person& p : people) ctx.remove(p);
    }
    {
        // Objects interleaved by type, named w0... (32 methods) and n0... (one method).
        Objects wide, narrow;
        make_objects(32, wide, std::make_integer_sequence<int, kParts>());
        make_objects(1, narrow, std::make_integer_sequence<int, kParts>());
        std::mt19937 rng(7);
        std::string text;
        for (size_t i = 0; i < wide.size(); ++i) {
            if (!ctx.add(*wide[i], baker::Symbol("w" + std::to_string(i)))) return 1;
            if (!ctx.add(*narrow[i], baker::Symbol("n" + std::to_string(i)))) return 1;
            text += "{w" + std::to_string(i) + " m" + std::to_string(rng() % 32) + " 0} ";
            text += "{n" + std::to_string(i) + " m0 0} ";
        }
        auto cold = baker::read_string(text, "types.dta");
        if (!cold) return 1;
        std::vector<uint32_t> w_order(wide.size()), n_order(wide.size());
        for (uint32_t i = 0; i < wide.size(); ++i) w_order[i] = 2 * i, n_order[i] = 2 * i + 1;
        std::shuffle(w_order.begin(), w_order.end(), rng);
        std::shuffle(n_order.begin(), n_order.end(), rng);
        auto run = [&](const std::vector<uint32_t>& which) {
            return ns_per_call([&](int i) {
                auto r = ctx.eval((**cold)[which[static_cast<unsigned>(i) % which.size()]]);
                return r ? static_cast<double>(r->try_float().value_or(0)) : -1.0;
            });
        };
        std::printf("%-38s %7.2f ns\n", "{nN m0 0}: 64 types, one method", run(n_order));
        std::printf("%-38s %7.2f ns\n", "{wN mK 0}: 64 types, 32 methods", run(w_order));
        std::vector<baker::ObjectBase*> shuffled;
        for (auto& o : narrow) shuffled.push_back(o.get());
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        std::printf(
            "%-38s %7.2f ns\n", "handle(HitMsg): 64 types, shuffled", ns_per_call([&](int i) {
                auto r =
                    shuffled[static_cast<unsigned>(i) % shuffled.size()]->handle(HitMsg{i & 1});
                return r ? static_cast<double>(r->try_int().value_or(0)) : -1.0;
            }));
    }

    // Resolving a name: the Context's table, indexed by Symbol::index(), against a hash map.
    std::vector<baker::Symbol> keys;
    std::unordered_map<baker::Symbol, int> map;
    std::vector<int> table;
    for (int i = 0; i < 200; ++i) {
        keys.emplace_back("fn" + std::to_string(i));
        map[keys.back()] = i;
        if (keys.back().index() >= table.size()) table.resize(keys.back().index() + 1);
        table[keys.back().index()] = i;
    }
    std::printf("\nresolving a function name among 200\n");
    std::printf("%-38s %7.2f ns\n", "table by Symbol::index()", ns_per_call([&](int i) {
                    const uint32_t ix = opaque(keys[static_cast<size_t>(i % 200)]).index();
                    return ix < table.size() ? double(table[ix]) : -1.0;
                }));
    std::printf("%-38s %7.2f ns\n", "std::unordered_map<Symbol>", ns_per_call([&](int i) {
                    return double(map.find(opaque(keys[static_cast<size_t>(i % 200)]))->second);
                }));
    return 0;
}
