#pragma once

// Tasks: work that runs over time, as C++20 coroutines ("Tasks").
//
//   baker::Task blink(baker::Scheduler& s, Light& light) {
//       for (int i = 0; i < 3; ++i) {
//           light.on();
//           co_await baker::delay(100);
//           light.off();
//           co_await baker::until([] { return player_ready(); });
//       }
//   }
//   auto h = ctx.scheduler().spawn(blink(ctx.scheduler(), light), "blink");
//   ctx.scheduler().tick(dt);   // once a frame, in the host's units
//
// A task's frame comes from its Scheduler's pool, never the heap:
// a coroutine must take a Scheduler& or a Context& (its default Scheduler) among its
// parameters, or it does not compile. A frame larger than the pool's budget makes spawn()
// fail with a Limit error. A spawned task starts at the next tick. The library owns no
// thread and no clock: the host calls tick().
//
// Scripts reach a Scheduler as an object: a Context's default one is named `scheduler`.
//   {scheduler delay_task 2 {play_sfx "ding"}}           ; once, 2 units from now
//   {scheduler interp_task $frame 0 100 {seek $frame}}   ; $frame 0..100 over 100 units
//   {scheduler thread_task {open_door} {wait {door_open}} {wait_for 1} {close_door}}
// A script task captures, when made, the value of every bound $variable it names (not
// globals), and sees those values each step. Made in a handler, it ends when its
// $this object is removed. Each returns a handle for {scheduler cancel $h}.

#include <baker/error.hpp>
#include <baker/node.hpp>
#include <baker/object.hpp>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace baker {

class Context;
class Scheduler;

// A task, as a salted index (as ObjectHandle is): safe to keep after the task ends.
struct TaskHandle {
    uint64_t bits = 0;

    [[nodiscard]] static constexpr TaskHandle make(uint32_t index, uint32_t salt) noexcept {
        return {static_cast<uint64_t>(index) | (static_cast<uint64_t>(salt) << 32)};
    }
    [[nodiscard]] constexpr uint32_t index() const noexcept {
        return static_cast<uint32_t>(bits & 0xFFFFFFFFu);
    }
    [[nodiscard]] constexpr uint32_t salt() const noexcept {
        return static_cast<uint32_t>(bits >> 32);
    }
    friend constexpr bool operator==(TaskHandle, TaskHandle) noexcept = default;
};

namespace detail {

template <class A>
inline constexpr bool is_task_home = std::is_same_v<std::remove_cvref_t<A>, Scheduler> ||
                                     std::is_same_v<std::remove_cvref_t<A>, Context>;

[[nodiscard]] Scheduler& scheduler_of(Context& ctx) noexcept;
inline Scheduler& scheduler_of(Scheduler& s) noexcept {
    return s;
}
// The Scheduler named by the first Scheduler& or Context& among a coroutine's arguments.
template <class First, class... Rest>
Scheduler& task_home(First& first, [[maybe_unused]] Rest&... rest) noexcept {
    if constexpr (is_task_home<First>)
        return scheduler_of(first);
    else
        return task_home(rest...);
}

// The pool (task.cpp): a block of the Scheduler's frame budget, or null if `size` exceeds
// it (the size is recorded for spawn's error).
[[nodiscard]] void* task_frame(Scheduler& s, size_t size) noexcept;
void free_task_frame(void* frame) noexcept;

// What every task promise holds: its Scheduler, and its slot once spawned.
struct TaskPromiseBase {
    Scheduler* scheduler = nullptr;
    uint32_t slot = UINT32_MAX;
};
// Parking a suspended task until it is due (task.cpp).
void park_delay(TaskPromiseBase& p, double t) noexcept;
void park_until(TaskPromiseBase& p, bool (*poll)(void*), void* waiter) noexcept;
void park_tick(TaskPromiseBase& p) noexcept;

}  // namespace detail

class Task {
public:
    struct promise_type : detail::TaskPromiseBase {
        // The only constructor, and the only operator new, need a Scheduler or a Context
        // among the coroutine's arguments. Both: GCC enforces it through operator new, but
        // Clang 18 falls back to the global one, and only the constructor stops it.
        template <class... A>
            requires(detail::is_task_home<A> || ...)
        explicit promise_type(A&... a) noexcept : TaskPromiseBase{&detail::task_home(a...)} {}

        template <class... A>
            requires(detail::is_task_home<A> || ...)
        static void* operator new(size_t size, A&... a) noexcept {
            return detail::task_frame(detail::task_home(a...), size);
        }
        static void operator delete(void* frame, size_t) noexcept {
            detail::free_task_frame(frame);
        }
        static Task get_return_object_on_allocation_failure() noexcept { return Task(); }

        Task get_return_object() noexcept {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept;  // never: Baker builds without exceptions
    };

    Task() noexcept = default;
    Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task& operator=(Task&& o) noexcept {
        Task(std::move(o)).swap(*this);
        return *this;
    }
    ~Task() {
        if (h_) h_.destroy();  // never spawned
    }
    void swap(Task& o) noexcept { std::swap(h_, o.h_); }
    // False if its frame did not fit the pool.
    explicit operator bool() const noexcept { return static_cast<bool>(h_); }

private:
    friend class Scheduler;
    explicit Task(std::coroutine_handle<promise_type> h) noexcept : h_(h) {}
    std::coroutine_handle<promise_type> h_;
};

// --- Awaitables. Each lives in the suspended frame, so waiting allocates nothing. ---

// Resumes the task once the Scheduler's time has advanced by `t`. Not at all if t <= 0.
struct Delay {
    double t;
    [[nodiscard]] bool await_ready() const noexcept { return t <= 0; }
    void await_suspend(std::coroutine_handle<Task::promise_type> h) const noexcept {
        detail::park_delay(h.promise(), t);
    }
    void await_resume() const noexcept {}
};
[[nodiscard]] inline Delay delay(double t) noexcept {
    return {t};
}

// Resumes the task at the first tick where `predicate()` is true. Polled each tick.
template <class P>
struct Until {
    P predicate;
    [[nodiscard]] bool await_ready() { return static_cast<bool>(predicate()); }
    void await_suspend(std::coroutine_handle<Task::promise_type> h) noexcept {
        detail::park_until(h.promise(), &poll, this);
    }
    void await_resume() const noexcept {}
    static bool poll(void* self) {
        return static_cast<bool>(static_cast<Until*>(self)->predicate());
    }
};
template <class P>
[[nodiscard]] Until<P> until(P predicate) {
    return {std::move(predicate)};
}

// Resumes the task at the next tick.
struct NextTick {
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<Task::promise_type> h) const noexcept {
        detail::park_tick(h.promise());
    }
    void await_resume() const noexcept {}
};
[[nodiscard]] inline NextTick next_tick() noexcept {
    return {};
}

struct SchedulerOptions {
    uint32_t frame_bytes = 4096;  // the frame budget: the largest task frame
    uint32_t chunk = 64;          // blocks taken from the Context's allocator at a time
    uint32_t max_tasks = 0;       // live tasks allowed at once; 0: no limit
};

struct SchedulerStats {
    uint32_t live = 0;           // tasks spawned and not finished
    uint32_t peak = 0;           // the most live at once
    uint32_t resumed = 0;        // tasks the last tick resumed
    uint32_t blocks = 0;         // frame blocks the pool holds, in use or free
    uint32_t largest_frame = 0;  // the largest frame asked for, in bytes
};

// Runs tasks on one timeline, in time units the host chooses. A Context has a default
// Scheduler (Context::scheduler()); a game with game time and UI time makes another.
class Scheduler : public Object<Scheduler> {
public:
    explicit Scheduler(Context& ctx, SchedulerOptions options = {});
    ~Scheduler() override;  // destroys every task's frame, running its locals' destructors
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Takes over `task`, which starts at the next tick. A Limit error if its frame did not
    // fit the budget, or max_tasks are live; `name` labels it in errors and statistics.
    [[nodiscard]] Expected<TaskHandle> spawn(Task task, Symbol name = {});
    // Ends a task: at once outside a tick, at the end of the tick inside one.
    void cancel(TaskHandle task) noexcept;
    [[nodiscard]] bool running(TaskHandle task) const noexcept;

    // Advances time by `dt` and resumes every task that is due, in the order of (wake
    // time, spawn order); then applies what the tasks deferred.
    void tick(double dt);
    [[nodiscard]] double now() const noexcept;
    [[nodiscard]] SchedulerStats stats() const noexcept;
    static void bind(Binder<Scheduler>& b);  // the script commands (script_tasks.cpp)

    struct Impl;
    [[nodiscard]] Impl& impl() const noexcept { return *impl_; }

private:
    Impl* impl_;
};

}  // namespace baker
