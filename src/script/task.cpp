// Tasks: the frame pool, the Scheduler's table of tasks, and the tick.

#include <baker/task.hpp>

#include "common/containers.hpp"
#include "internal.hpp"

#include <algorithm>
#include <cstdlib>

namespace baker {

namespace {
// A block of the pool: this header, then the frame, aligned as operator new aligns.
struct alignas(__STDCPP_DEFAULT_NEW_ALIGNMENT__) BlockHeader {
    Scheduler::Impl* pool;
    BlockHeader* next_free;
};
constexpr size_t kHeader = sizeof(BlockHeader);

enum class Wait : uint8_t { Free, Pending, Running, Delay, Until, Tick };

struct TaskSlot {
    std::coroutine_handle<Task::promise_type> h;
    uint32_t salt = 1;
    Wait wait = Wait::Free;
    bool cancelled = false;
    uint64_t seq = 0;  // spawn order, for a deterministic order within a tick
    bool (*poll)(void*) = nullptr;
    void* waiter = nullptr;
};

struct Due {
    double wake;
    uint64_t seq;
    uint32_t slot;
    uint32_t salt;
};
// A min-heap on (wake, seq): std's heap functions keep the largest first.
bool later(const Due& a, const Due& b) noexcept {
    return a.wake != b.wake ? a.wake > b.wake : a.seq > b.seq;
}
}  // namespace

struct Scheduler::Impl {
    Impl(Scheduler& s, Context& c, SchedulerOptions o)
        : self(s), ctx(c), alloc(c.impl().alloc), options(o) {
        if (options.chunk == 0) options.chunk = 1;  // the size grow() and ~Scheduler agree on
    }

    Scheduler& self;
    Context& ctx;
    const Allocator& alloc;
    SchedulerOptions options;
    double now = 0;
    uint64_t next_seq = 0;
    bool ticking = false;
    uint32_t running = UINT32_MAX;  // the slot being resumed

    Vec<TaskSlot> slots{alloc};
    Vec<uint32_t> free_slots{alloc};
    Vec<uint32_t> pending{alloc};  // spawned, to start at the next tick
    Vec<Due> delays{alloc};        // a heap
    Vec<uint32_t> untils{alloc};
    Vec<uint32_t> ticks{alloc};
    Vec<Due> ready{alloc};  // this tick's batch; reused
    Vec<uint32_t> cancels{alloc};

    // The pool: chunks from the Context's allocator, and a free list of blocks.
    Vec<void*> chunks{alloc};
    BlockHeader* free_blocks = nullptr;
    uint32_t refused = 0;  // the last frame size the pool refused, for spawn's error
    SchedulerStats stats;

    [[nodiscard]] size_t block_bytes() const noexcept {
        const size_t a = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
        return (kHeader + options.frame_bytes + a - 1) / a * a;
    }

    void* frame(size_t size) noexcept {
        if (size > stats.largest_frame) stats.largest_frame = static_cast<uint32_t>(size);
        if (size > options.frame_bytes) {
            refused = static_cast<uint32_t>(size);
            return nullptr;
        }
        if (!free_blocks) grow();
        BlockHeader* b = free_blocks;
        free_blocks = b->next_free;
        return reinterpret_cast<char*>(b) + kHeader;
    }
    void grow() {
        const size_t n = options.chunk;
        auto* mem = static_cast<char*>(
            detail::allocate(alloc, n * block_bytes(), __STDCPP_DEFAULT_NEW_ALIGNMENT__));
        chunks.push_back(mem);
        for (size_t i = n; i-- > 0;) {
            auto* b = ::new (mem + i * block_bytes()) BlockHeader{this, free_blocks};
            free_blocks = b;
        }
        stats.blocks += static_cast<uint32_t>(n);
    }

    [[nodiscard]] TaskSlot* slot(TaskHandle t) noexcept {
        const uint32_t i = t.index();
        return i < slots.size() && slots[i].wait != Wait::Free && slots[i].salt == t.salt()
                   ? &slots[i]
                   : nullptr;
    }

    void finish(uint32_t i) noexcept {
        TaskSlot& s = slots[i];
        s.h.destroy();
        TaskSlot fresh;
        fresh.salt = s.salt + 1 == 0 ? 1 : s.salt + 1;
        s = fresh;
        free_slots.push_back(i);
        --stats.live;
    }

    void resume(uint32_t i) {
        TaskSlot& s = slots[i];
        s.wait = Wait::Running;
        running = i;
        s.h.resume();
        running = UINT32_MAX;
        ++stats.resumed;
        TaskSlot& after = slots[i];  // resume may have spawned, and grown the table
        if (after.h.done()) {
            finish(i);
        } else if (after.wait == Wait::Running) {
            after.wait = Wait::Tick;  // suspended on something else: treat as next tick
            ticks.push_back(i);
        }
    }

    // Applies what this tick deferred: cancels.
    void sync() noexcept {
        for (uint32_t i : cancels)
            if (slots[i].wait != Wait::Free) finish(i);
        cancels.clear();
    }
};

namespace detail {

Scheduler& scheduler_of(Context& ctx) noexcept {
    return ctx.scheduler();
}

void* task_frame(Scheduler& s, size_t size) noexcept {
    return s.impl().frame(size);
}

void free_task_frame(void* frame) noexcept {
    auto* b = reinterpret_cast<BlockHeader*>(static_cast<char*>(frame) - kHeader);
    b->next_free = b->pool->free_blocks;
    b->pool->free_blocks = b;
}

namespace {
Scheduler::Impl& impl_of(TaskPromiseBase& p) noexcept {
    return p.scheduler->impl();
}
}  // namespace

void park_delay(TaskPromiseBase& p, double t) noexcept {
    Scheduler::Impl& s = impl_of(p);
    TaskSlot& slot = s.slots[p.slot];
    slot.wait = Wait::Delay;
    s.delays.push_back(Due{s.now + t, slot.seq, p.slot, slot.salt});
    std::push_heap(s.delays.begin(), s.delays.end(), later);
}

void park_until(TaskPromiseBase& p, bool (*poll)(void*), void* waiter) noexcept {
    Scheduler::Impl& s = impl_of(p);
    TaskSlot& slot = s.slots[p.slot];
    slot.wait = Wait::Until;
    slot.poll = poll;
    slot.waiter = waiter;
    s.untils.push_back(p.slot);
}

void park_tick(TaskPromiseBase& p) noexcept {
    Scheduler::Impl& s = impl_of(p);
    s.slots[p.slot].wait = Wait::Tick;
    s.ticks.push_back(p.slot);
}

}  // namespace detail

void Task::promise_type::unhandled_exception() noexcept {
    std::abort();
}

Scheduler::Scheduler(Context& ctx, SchedulerOptions options)
    : impl_(detail::create<Impl>(ctx.impl().alloc, *this, ctx, options)) {}

Scheduler::~Scheduler() {
    for (uint32_t i = 0; i < impl_->slots.size(); ++i)
        if (impl_->slots[i].wait != Wait::Free) impl_->slots[i].h.destroy();
    const Allocator& alloc = impl_->alloc;
    const size_t bytes = impl_->options.chunk * impl_->block_bytes();
    for (void* c : impl_->chunks) detail::deallocate(alloc, c, bytes);
    detail::destroy(alloc, impl_);
}

Expected<TaskHandle> Scheduler::spawn(Task task, Symbol name) {
    Impl& s = *impl_;
    // Built only on the error paths: spawning allocates nothing.
    auto who = [&] { return name.empty() ? Str("a task") : "task '" + Str(name.str()) + "'"; };
    if (!task)
        return Unexpected(make_error(ErrorCode::Limit, who() + " needs a frame of " +
                                                           detail::decimal(s.refused) +
                                                           " bytes; the Scheduler's budget is " +
                                                           detail::decimal(s.options.frame_bytes)));
    if (task.h_.promise().scheduler != this)
        return Unexpected(make_error(
            ErrorCode::Refused, who() + " was made for another Scheduler, whose pool holds it"));
    if (s.options.max_tasks && s.stats.live >= s.options.max_tasks)
        return Unexpected(make_error(
            ErrorCode::Limit,
            who() + " would exceed max_tasks (" + detail::decimal(s.options.max_tasks) + ")"));
    uint32_t i;
    if (!s.free_slots.empty()) {
        i = s.free_slots.back();
        s.free_slots.pop_back();
    } else {
        i = static_cast<uint32_t>(s.slots.size());
        s.slots.emplace_back();
    }
    TaskSlot& slot = s.slots[i];
    slot.h = std::exchange(task.h_, {});
    slot.wait = Wait::Pending;
    slot.seq = s.next_seq++;
    slot.h.promise().slot = i;
    s.pending.push_back(i);
    if (++s.stats.live > s.stats.peak) s.stats.peak = s.stats.live;
    return TaskHandle::make(i, slot.salt);
}

void Scheduler::cancel(TaskHandle task) noexcept {
    TaskSlot* s = impl_->slot(task);
    if (!s || s->cancelled) return;
    s->cancelled = true;
    impl_->cancels.push_back(task.index());
    if (!impl_->ticking) impl_->sync();
}

bool Scheduler::running(TaskHandle task) const noexcept {
    const TaskSlot* s = impl_->slot(task);
    return s && !s->cancelled;
}

void Scheduler::tick(double dt) {
    Impl& s = *impl_;
    s.now += dt;
    s.ticking = true;
    ++s.ctx.impl().ticking;
    s.stats.resumed = 0;
    s.ready.clear();
    // Times accumulate rounding (ten ticks of 0.1 make 0.9999999999999999), so a wake time
    // within a hair of now counts as due.
    const double due = s.now + 1e-9 * std::max(1.0, s.now < 0 ? -s.now : s.now);
    // A slot is taken once: a cancelled task's slot can be reused, and listed twice.
    auto take = [&](uint32_t i, double wake) {
        TaskSlot& slot = s.slots[i];
        if (slot.cancelled || slot.wait == Wait::Free || slot.wait == Wait::Running) return;
        slot.wait = Wait::Running;
        s.ready.push_back(Due{wake, slot.seq, i, slot.salt});
    };
    for (uint32_t i : s.pending) take(i, s.now);
    s.pending.clear();
    while (!s.delays.empty() && s.delays.front().wake <= due) {
        std::pop_heap(s.delays.begin(), s.delays.end(), later);
        const Due d = s.delays.back();
        s.delays.pop_back();
        if (s.slots[d.slot].salt == d.salt && s.slots[d.slot].wait == Wait::Delay)
            take(d.slot, d.wake);
    }
    for (size_t k = 0; k < s.untils.size();) {
        const uint32_t i = s.untils[k];
        const TaskSlot& slot = s.slots[i];
        if (slot.wait == Wait::Until && !slot.cancelled && !slot.poll(slot.waiter)) {  // not yet
            ++k;
            continue;
        }
        // The predicate may have spawned a task and moved the slots, so look again.
        if (s.slots[i].wait == Wait::Until && !s.slots[i].cancelled) take(i, s.now);
        s.untils[k] = s.untils.back();
        s.untils.pop_back();
    }
    for (uint32_t i : s.ticks)
        if (s.slots[i].wait == Wait::Tick) take(i, s.now);
    s.ticks.clear();
    // Usually in order already (tasks that wait a tick each time come back in the order they
    // ran), so check before sorting.
    auto before = [](const Due& a, const Due& b) {
        return a.wake != b.wake ? a.wake < b.wake : a.seq < b.seq;
    };
    if (!std::is_sorted(s.ready.begin(), s.ready.end(), before))
        std::sort(s.ready.begin(), s.ready.end(), before);
    for (size_t k = 0; k < s.ready.size(); ++k) {
        const Due d = s.ready[k];
        if (s.slots[d.slot].salt == d.salt && s.slots[d.slot].wait == Wait::Running)
            s.resume(d.slot);
    }
    s.ticking = false;
    s.sync();
    if (--s.ctx.impl().ticking == 0) s.ctx.sync();
}

Scheduler& Context::scheduler() {
    return *impl_->scheduler;
}

double Scheduler::now() const noexcept {
    return impl_->now;
}

SchedulerStats Scheduler::stats() const noexcept {
    return impl_->stats;
}

}  // namespace baker
