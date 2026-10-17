// Tasks (M7 plan). Targets: a tick with 10,000 tasks asleep and none due under
// 100 ns; resuming a C++ task <= 20 ns; spawning allocates nothing once the pool is warm.
#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/task.hpp>

#include "alloc_counter.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace {

template <class F>
double best_ns(int reps, int per_rep, F&& f) {
    double best = 1e30;
    for (int r = 0; r < reps; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / per_rep);
    }
    return best;
}

baker::Task sleeper(baker::Scheduler&) {
    co_await baker::delay(1e9);
}
baker::Task stepper(baker::Scheduler&, long* n) {
    for (;;) {
        ++*n;
        co_await baker::next_tick();
    }
}
baker::Task once(baker::Scheduler&, long* n) {
    ++*n;
    co_return;
}

}  // namespace

int main() {
    baker::Context ctx;
    {
        baker::Scheduler s(ctx);
        for (int i = 0; i < 10'000; ++i) (void)s.spawn(sleeper(s));
        s.tick(0);  // start them: each parks on its delay
        const double ns = best_ns(7, 10'000, [&] {
            for (int i = 0; i < 10'000; ++i) s.tick(0.001);
        });
        std::printf("%-44s %8.2f ns   (target < 100 ns)\n", "tick, 10,000 asleep, none due", ns);
    }
    {
        baker::Scheduler s(ctx);
        long n = 0;
        for (int i = 0; i < 1'000; ++i) (void)s.spawn(stepper(s, &n));
        s.tick(0);
        const double ns = best_ns(7, 1'000 * 100, [&] {
            for (int i = 0; i < 100; ++i) s.tick(1);
        });
        std::printf("%-44s %8.2f ns   (target <= 20 ns)\n",
                    "resume, 1,000 tasks on next_tick, per task", ns);
        std::printf("%-44s %8u bytes\n", "that task's frame", s.stats().largest_frame);
    }
    {
        baker::Scheduler s(ctx);
        long n = 0;
        for (int i = 0; i < 1'000; ++i) (void)s.spawn(once(s, &n));
        s.tick(0);  // warms the pool and the tables
        baker::test::AllocationScope scope;
        const double ns = best_ns(7, 1'000, [&] {
            for (int i = 0; i < 1'000; ++i) (void)s.spawn(once(s, &n));
            s.tick(0);
        });
        std::printf("%-44s %8.2f ns   %zu allocations\n", "spawn + run to completion, per task", ns,
                    scope.count());
    }
    {
        // Script tasks: a thread_task's step, beside the same command evaluated directly.
        if (!ctx.func("add", [](int64_t a, int64_t b) { return a + b; })) return 1;
        auto tree = baker::read_string(
            "{add 1 2} {scheduler thread_task {add 1 2} {wait_tick} {add 1 2} {wait_tick} "
            "{add 1 2} {wait_tick} {add 1 2} {wait_tick} {add 1 2}} "
            "{do (($a 1) ($b 2)) {scheduler delay_task 1e9 {add $a $b}}}",
            "bench.dta");
        if (!tree) return 1;
        const baker::Node& add = (**tree)[0];
        const baker::Node& thread = (**tree)[1];
        const baker::Node& later = (**tree)[2];
        std::printf("%-44s %8.2f ns\n", "{add 1 2}, evaluated directly", best_ns(7, 100'000, [&] {
                        for (int i = 0; i < 100'000; ++i) (void)ctx.eval(add);
                    }));
        baker::Scheduler& s = ctx.scheduler();
        for (int i = 0; i < 1'000; ++i) (void)ctx.eval(thread);
        s.tick(0);
        const double step = best_ns(1, 1'000 * 4, [&] {
            for (int i = 0; i < 4; ++i) s.tick(1);
        });
        std::printf("%-44s %8.2f ns\n", "thread_task step: {add 1 2} then {wait_tick}", step);
        baker::test::AllocationScope scope;
        const double make = best_ns(1, 1'000, [&] {
            for (int i = 0; i < 1'000; ++i) (void)ctx.eval(later);
        });
        std::printf("%-44s %8.2f ns   %zu allocations\n",
                    "making a delay_task capturing 2 variables", make, scope.count());
        std::printf("%-44s %8u bytes\n", "the largest frame: a script task's",
                    s.stats().largest_frame);
    }
    return 0;
}
