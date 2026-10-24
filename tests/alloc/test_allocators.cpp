// A Context and a Config allocate only through Baker allocators. This binary's main
// (main.cpp) installs a malloc default allocator before anything else runs, so global
// operator new can be watched on its own.

#include <doctest.h>

#include <baker/config.hpp>
#include <baker/object.hpp>
#include <baker/script.hpp>
#include <baker/task.hpp>
#include <baker/tools/console.hpp>
#include <baker/tools/layers.hpp>
#include <baker/tweak.hpp>

#include "alloc_counter.hpp"
#include "dev_only.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

extern std::atomic<size_t> g_default_allocations;  // main.cpp

namespace {

// Counts through malloc, never operator new.
struct Counting {
    size_t allocs = 0;
    size_t frees = 0;
    size_t live_bytes = 0;

    [[nodiscard]] baker::Allocator allocator() {
        return {[](void* u, size_t size, size_t) -> void* {
                    auto& c = *static_cast<Counting*>(u);
                    ++c.allocs;
                    c.live_bytes += size;
                    return std::malloc(size);
                },
                [](void* u, void* p, size_t size) {
                    auto& c = *static_cast<Counting*>(u);
                    ++c.frees;
                    c.live_bytes -= size;
                    std::free(p);
                },
                this};
    }
};

// Files the Config reads, by name.
const char* text_of(std::string_view path) {
    if (path == "base.dta")
        return "(window (width 640) (height 480)) (speed 2.5)\n#include extra.dta\n";
    if (path == "extra.dta") return "(title \"baker\") (list 1 2 3 4 5 6 7 8 9 10)";
    if (path == "over.dta") return "(speed 4.0) (window (width 800))";
    return nullptr;
}

bool read_text(std::string_view path, baker::TextSink out) {
    const char* t = text_of(path);
    if (!t) return false;
    out(t);
    return true;
}

bool discard(std::string_view, std::string_view) {
    return true;
}

struct Mover : baker::Object<Mover> {
    float x = 0;
    float step(float by) { return x += by; }
    static void bind(baker::Binder<Mover>& b) {
        b.method("step", &Mover::step);
        b.property("x", &Mover::x);
    }
};

// A file whose text changes with `g_version`, for reloads.
int g_version = 0;
bool read_versioned(std::string_view path, baker::TextSink out) {
    if (path != "live.dta") return false;
    char text[64];
    const int n =
        std::snprintf(text, sizeof text, "(speed %d) (list 1 2 3 \"s%d\")", g_version, g_version);
    out(std::string_view(text, static_cast<size_t>(n)));
    return true;
}

baker::Task counter(baker::Scheduler&, int* n) {
    for (int i = 0; i < 3; ++i) {
        ++*n;
        co_await baker::delay(1);
    }
}

int add(int a, int b) {
    return a + b;
}

}  // namespace

TEST_CASE("a Config allocates through its allocator, and nothing through operator new") {
    Counting mine;
    const baker::Allocator alloc = mine.allocator();
    size_t global = 0, unowned = 0;
    bool ok = false;
    float speed_read = 0;
    {
        const baker::test::AllocationScope scope;  // no doctest assertion inside: they allocate
        const size_t unowned_before = g_default_allocations.load();
        {
            baker::TweakRegistry tweaks;
            baker::Tweak<float> speed{"speed", 1.0f, {}, tweaks};
            baker::Config config({.tweaks = &tweaks}, alloc);
            config.session().set_reader(&read_text);
            config.set_writer(&discard);
            ok = config.push_layer("base", "base.dta").has_value();
            ok = ok && config.push_layer("over", "over.dta").has_value();
            ok = ok && config.get<int>(baker::Path("window/width")).value_or(0) == 800;
            ok = ok && baker::tools::explain(config, baker::Path("window/width")).has_value();
            ok = ok && baker::tools::explain(config, baker::Path("speed"))
                           .has_value();  // the code default
            const baker::LayerPolicy policy{.data_only = true, .existing_only = true};
            const auto checked = config.check_layer_text("hotfix", "(window (width 1024))", policy);
            ok = ok && checked.has_value() && checked->ok() && checked->effect.size() == 1;
            if constexpr (!baker::kShipping) {  // live edits are refused in shipping
                ok = ok && config.set("window/height", baker::Node(int64_t{600})).has_value();
                const baker::ConfigSnapshot snap = config.snapshot();
                ok = ok && config.reload().has_value();
                ok = ok && config.restore(snap).has_value();
                ok = ok && baker::tools::layer_effect(config, "over").has_value();
            }
            ok = ok && !config.get<int>(baker::Path("missing")).has_value();  // an error record
            ok = ok && config.remove_layer("over").has_value();
            speed_read = speed;
        }
        unowned = g_default_allocations.load() - unowned_before;
        global = scope.count();
    }
    CHECK(ok);
    CHECK(speed_read == 2.5f);
    MESSAGE("Config: " << unowned << " allocations through the default allocator");
    if (baker::test::counting_allocations()) CHECK(global == 0);  // not under TSan
    CHECK(mine.allocs > 0);
    CHECK(mine.allocs == mine.frees);
    CHECK(mine.live_bytes == 0);
    MESSAGE("Config: " << mine.allocs << " allocations through its allocator");
}

TEST_CASE("a LayerChecker keeps its state on its allocator, and nothing through operator new") {
    Counting mine, theirs;
    size_t global = 0;
    bool ok = false;
    {
        const baker::test::AllocationScope scope;  // no doctest assertion inside: they allocate
        baker::Config config({.tweaks = nullptr}, mine.allocator());
        config.session().set_reader(&read_text);
        ok = config.push_layer("base", "base.dta").has_value();
        const baker::LayerPolicy policy{.data_only = true, .existing_only = true};
        {
            const baker::LayerChecker checker = config.checker(theirs.allocator());
            const auto checked = checker.check_text("hotfix", "(window (width 1024))", policy);
            ok = ok && checked.has_value() && checked->ok() && checked->effect.size() == 1;
            ok = ok && config
                           .push_layer_text("hotfix", "(window (width 1024))",
                                            {.policy = &policy, .checked = &*checked})
                           .has_value();
        }
        ok = ok && config.get<int>(baker::Path("window/width")).value_or(0) == 1024;
        global = scope.count();
    }
    CHECK(ok);
    if (baker::test::counting_allocations()) CHECK(global == 0);  // not under TSan
    CHECK(theirs.allocs > 0);
    CHECK(theirs.allocs == theirs.frees);
    CHECK(theirs.live_bytes == 0);
    CHECK(mine.allocs == mine.frees);
    MESSAGE("LayerChecker: " << theirs.allocs << " allocations through its allocator");
}

TEST_CASE("a Context allocates through its allocator, and nothing through operator new") {
    Counting mine;
    const baker::Allocator alloc = mine.allocator();
    size_t global = 0, unowned = 0;
    bool ok = false;
    {
        const baker::test::AllocationScope scope;
        const size_t unowned_before = g_default_allocations.load();
        {
            baker::Context ctx(alloc);
            ok = ctx.func("add", &add).has_value();
            ctx.add_type<Mover>();
            Mover bob;
            ok = ok && ctx.add(bob, "bob").has_value();
            ok = ok && ctx.eval("{func twice ($a) {add $a $a}} {set $n {twice 21}}").has_value();
            ok = ok && ctx.eval("{class Ball (bounce ($h) {add $h 1})} {new Ball b1}").has_value();
            ok = ok && ctx.eval("{b1 bounce 3}").value_or(baker::Node()).try_int() == 4;
            ok = ok && ctx.eval("{bob step 1.5} {new Mover m2 (x 2)} {delete m2}").has_value();
            ok = ok && !ctx.eval("{add 1}").has_value();  // an error record
        }
        {
            // The Config's tools and the reply form (§4).
            baker::Config config({.tweaks = nullptr}, alloc);
            config.session().set_reader(&read_text);
            ok = ok && config.push_layer("base", "base.dta").has_value();
            baker::Context ctx(alloc);
            ok = ok && baker::tools::bind_config_tools(ctx, config).has_value();
            ok = ok && ctx.eval("{config.explain window.width}").has_value();
            if constexpr (!baker::kShipping)
                ok = ok && ctx.eval("{config.snapshot a} {config.restore a}").has_value();
            size_t reply_bytes = 0;  // a sink that keeps nothing: the reply's own work only
            baker::eval_request(
                ctx, "(1 {config.source window.width})",
                {[](void* n, std::string_view s) { *static_cast<size_t*>(n) += s.size(); },
                 &reply_bytes});
            ok = ok && reply_bytes > 0;
        }
        unowned = g_default_allocations.load() - unowned_before;
        global = scope.count();
    }
    CHECK(ok);
    MESSAGE("Context: " << unowned << " allocations through the default allocator");
    if (baker::test::counting_allocations()) CHECK(global == 0);  // not under TSan
    CHECK(mine.allocs > 0);
    CHECK(mine.allocs == mine.frees);
    CHECK(mine.live_bytes == 0);
    MESSAGE("Context: " << mine.allocs << " allocations through its allocator");
}

TEST_CASE("loose parameters are the Context's, and leave with the object") {
    Counting mine;
    const baker::Allocator alloc = mine.allocator();
    {
        Mover late;  // outlives the Context below
        baker::Context ctx(alloc);
        Mover warm, bob;
        REQUIRE(ctx.add(warm));  // the free-slot list's first growth, out of the way
        ctx.remove(warm);
        REQUIRE(ctx.add(bob, "bob"));
        const size_t before = mine.live_bytes;
        REQUIRE(ctx.eval("{bob set mood happy} {bob set pos (1 2)}"));
        CHECK(mine.live_bytes > before);  // through the Context's allocator
        CHECK(ctx.eval("{bob get mood}").value_or(baker::Node()).try_sym() ==
              baker::Symbol("happy"));
        ctx.remove(bob);
        CHECK(mine.live_bytes == before);  // freed as bob left
        REQUIRE(ctx.add(bob, "bob"));
        CHECK_FALSE(ctx.eval("{bob get mood}"));  // a new stay starts with none

        // An object that outlives its Context: the Context frees its parameters as it
        // goes, so nothing is left to free through an allocator that may be gone.
        REQUIRE(ctx.add(late, "late"));
        REQUIRE(ctx.eval("{late set mood calm}"));
    }
    CHECK(mine.allocs == mine.frees);
    CHECK(mine.live_bytes == 0);
}

BAKER_DEV_TEST_CASE("reloads free the trees they replace") {
    Counting mine;
    const baker::Allocator alloc = mine.allocator();
    size_t steady = 0;
    bool ok = true;
    {
        baker::Config config({.tweaks = nullptr}, alloc);
        config.session().set_reader(&read_versioned);
        ok = config.push_layer("live", "live.dta").has_value();
        for (int i = 1; i <= 50; ++i) {
            g_version = i;
            ok = ok && config.reload().value_or(false);
            if (i == 10) steady = mine.live_bytes;  // from here the text keeps its length
        }
        ok = ok && config.get<int>(baker::Path("speed")).value_or(0) == 50;
        CHECK(mine.live_bytes == steady);  // each reload's arena replaced the last
    }
    CHECK(ok);
    CHECK(mine.live_bytes == 0);
}

TEST_CASE("tasks allocate through the Context's allocator, and nothing through operator new") {
    Counting mine;
    const baker::Allocator alloc = mine.allocator();
    size_t global = 0;
    bool ok = false;
    int n = 0;
    {
        const baker::test::AllocationScope scope;
        {
            baker::Context ctx(alloc);
            baker::Scheduler& s = ctx.scheduler();
            ok = ctx.func("add", &add).has_value();
            for (int i = 0; i < 100; ++i) ok = ok && s.spawn(counter(s, &n), "counter").has_value();
            ok = ok && ctx.eval(
                              "{do (($a 1)) {scheduler thread_task {add $a 1} {wait_tick} "
                              "{add $a 2}}} {scheduler interp_task $f 0 3 {add $f 1}} "
                              "{scheduler delay_task 2 {add 1 1}}")
                           .has_value();
            for (int i = 0; i < 6; ++i) s.tick(1);
            ok = ok && s.stats().live == 0;
            baker::Scheduler other(ctx, {.frame_bytes = 1024, .chunk = 0});  // 0: one at a time
            ok = ok && other.spawn(counter(other, &n)).has_value();  // left running: freed with it
            other.tick(1);
        }
        global = scope.count();
    }
    CHECK(ok);
    CHECK(n == 301);
    if (baker::test::counting_allocations()) CHECK(global == 0);
    CHECK(mine.allocs == mine.frees);
    CHECK(mine.live_bytes == 0);
    MESSAGE("Context with tasks: " << mine.allocs << " allocations through its allocator");
}
