// Tasks: a C++ coroutine and the three script tasks, run by a host that ticks time.
//
//   baker_example_tasks
#include <baker/std_script.hpp>
#include <baker/task.hpp>

#include <cstdio>

namespace {

bool door_open = false;

// A C++ task. Its frame comes from the Scheduler's pool, which is why it takes the
// Scheduler (or a Context) as a parameter.
baker::Task open_door(baker::Scheduler& s) {
    std::printf("t=%g  door: unlocking\n", s.now());
    co_await baker::delay(2);
    door_open = true;
    std::printf("t=%g  door: open\n", s.now());
    co_await baker::until([] { return !door_open; });
    std::printf("t=%g  door: someone closed it\n", s.now());
}

}  // namespace

int main() {
    baker::Context ctx;
    if (!ctx.func("say",
                  [&ctx](std::string_view text) {
                      std::printf("t=%g  %.*s\n", ctx.scheduler().now(), int(text.size()),
                                  text.data());
                  }) ||
        !ctx.func("door_open", [] { return door_open; }) ||
        !ctx.func("close_door", [] { door_open = false; }))
        return 1;

    baker::Scheduler& s = ctx.scheduler();
    if (!s.spawn(open_door(s), "open_door")) return 1;

    // Script tasks: once later; a variable stepped over time; commands across ticks.
    auto r = ctx.eval(
        "{scheduler delay_task 1 {say \"one unit later\"}}\n"
        "{scheduler interp_task $fade 0 3 {say {if {== $fade 3} \"fade done\" \"fading\"}}}\n"
        "{scheduler thread_task {wait {door_open}} {say \"guard: walking in\"} {wait_for 1}\n"
        "  {say \"guard: closing the door\"} {close_door}}",
        "tasks.dta");
    if (!r) return std::printf("%s\n", baker::describe(r.error()).data()), 1;

    // The host owns time: one tick a frame, in its own units.
    for (int frame = 0; frame < 6; ++frame) s.tick(1);
    std::printf("tasks left: %u\n", s.stats().live);
    return 0;
}
