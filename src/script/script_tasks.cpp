// Script tasks: delay_task, interp_task, and thread_task, each a coroutine
// from its Scheduler's pool, so both sides share one tick.

#include <baker/task.hpp>

#include "common/containers.hpp"
#include "internal.hpp"

#include <algorithm>

namespace baker {
namespace {

// A task keeps its captures in its frame, so making one allocates nothing.
constexpr uint32_t kMaxCaptures = 16;

enum class Kind : uint8_t { Delay, Interp, Thread };

struct ScriptTask {
    Kind kind = Kind::Delay;
    Ref<Array> command;  // keeps the body, and its load, alive
    uint32_t first = 0;  // the body's first node in `command`
    double time = 0;     // delay_task's delay
    double from = 0;     // interp_task's range
    double to = 0;
    Symbol var;  // interp_task's variable
    Node self;   // $this when the task was made: the task ends with that object
    uint32_t count = 0;
    Symbol names[kMaxCaptures];
    Node values[kMaxCaptures];
};

// Records the value of every bound $variable `n` names, except globals, $this, and the
// interp variable: by value, when the task is made.
bool capture(const Node& n, Context::Impl& impl, ScriptTask& t) {
    if (const Array* a = n.array()) {
        for (const Node& child : *a)
            if (!capture(child, impl, t)) return false;
        return true;
    }
    const Symbol name = n.var_name();
    if (name.empty() || name == sym_this() || name == t.var) return true;
    if (std::find(t.names, t.names + t.count, name) != t.names + t.count) return true;
    if (std::any_of(impl.globals.begin(), impl.globals.end(),
                    [&](const Global& g) { return g.name == name; }))
        return true;
    const Node* cell = impl.cell_if(name);
    if (!cell || is_unset(*cell)) return true;
    if (t.count == kMaxCaptures) return false;
    t.names[t.count] = name;
    t.values[t.count++] = *cell;
    return true;
}

// Runs `nodes` with the task's captures bound, then keeps their values, so the task's own
// {set}s persist from step to step.
Expected<Node> step(Context& ctx, ScriptTask& t, std::span<const Node> nodes,
                    const Node* interp = nullptr) {
    Context::Impl& impl = ctx.impl();
    Rebind rebind(impl);
    if (t.self.type() == Type::Object) impl.bind(sym_this(), t.self);
    for (uint32_t i = 0; i < t.count; ++i) impl.bind(t.names[i], t.values[i]);
    if (interp) impl.bind(t.var, *interp);
    auto r = ctx.run(nodes);
    for (uint32_t i = 0; i < t.count; ++i) t.values[i] = *impl.cell(t.names[i]);
    return r;
}

Symbol kind_name(Kind k) {
    static const Symbol names[] = {Symbol("delay_task"), Symbol("interp_task"),
                                   Symbol("thread_task")};
    return names[static_cast<int>(k)];
}

// Ends the task with `r`'s error, if it has one: to the Context's notice sink, with the
// task on its stack.
bool ok(Context& ctx, const ScriptTask& t, Expected<Node>& r) {
    if (r) return true;
    Error e = std::move(r).error();
    e.push_stack(StackEntry{kind_name(t.kind), {}, t.command->file(), t.command->line()});
    ctx.notice(e);
    return false;
}

bool alive(Context& ctx, const ScriptTask& t) {
    return t.self.type() != Type::Object || ctx.try_resolve(t.self) != nullptr;
}

// A {wait ...} form, and which one.
enum class WaitForm : uint8_t { None, Until, For, Tick };
WaitForm wait_form(const Node& n) {
    static const Symbol wait("wait"), wait_for("wait_for"), wait_tick("wait_tick");
    const Array* a = n.type() == Type::Command ? n.array() : nullptr;
    const auto head = a && !a->empty() ? (*a)[0].try_sym() : std::nullopt;
    if (!head) return WaitForm::None;
    if (*head == wait && a->size() == 2) return WaitForm::Until;
    if (*head == wait_for && a->size() == 2) return WaitForm::For;
    if (*head == wait_tick && a->size() == 1) return WaitForm::Tick;
    return WaitForm::None;
}

// The value of `n` as a number, evaluated with the task's captures; an error ends the task.
std::optional<double> number(Context& ctx, ScriptTask& t, const Node& n, std::string_view what) {
    auto r = step(ctx, t, std::span<const Node>(&n, 1));
    if (r && !r->try_float())
        r = Unexpected(make_error(ErrorCode::Type,
                                  Str(what) + " is " + Str(type_name(r->type())) + ", not a number",
                                  t.command->file(), t.command->line()));
    if (!ok(ctx, t, r)) return std::nullopt;
    return r->try_float();
}

Task run(Scheduler& s, Context& ctx, ScriptTask t) {
    const std::span<const Node> body = t.command->nodes().subspan(t.first);
    switch (t.kind) {
        case Kind::Delay: {
            co_await delay(t.time);
            if (!alive(ctx, t)) co_return;
            auto r = step(ctx, t, body);
            (void)ok(ctx, t, r);
            co_return;
        }
        case Kind::Interp: {
            // From `from` to `to` over |to - from| units: `$frame` counts one per unit.
            const double start = s.now();
            const double span = t.to > t.from ? t.to - t.from : t.from - t.to;
            for (;;) {
                if (!alive(ctx, t)) co_return;
                const double k = span > 0 ? std::min(1.0, (s.now() - start) / span) : 1.0;
                const Node value(k >= 1 ? t.to : t.from + (t.to - t.from) * k);
                auto r = step(ctx, t, body, &value);
                if (!ok(ctx, t, r) || k >= 1) co_return;
                co_await next_tick();
            }
        }
        case Kind::Thread: {
            for (const Node& cmd : body) {
                if (!alive(ctx, t)) co_return;
                switch (wait_form(cmd)) {
                    case WaitForm::Until:
                        for (;;) {
                            const auto v = number(ctx, t, (*cmd.array())[1], "wait's condition");
                            if (!v) co_return;
                            if (*v != 0) break;
                            co_await next_tick();
                            if (!alive(ctx, t)) co_return;
                        }
                        break;
                    case WaitForm::For: {
                        const auto v = number(ctx, t, (*cmd.array())[1], "wait_for's time");
                        if (!v) co_return;
                        co_await delay(*v);
                        break;
                    }
                    case WaitForm::Tick: co_await next_tick(); break;
                    case WaitForm::None: {
                        auto r = step(ctx, t, std::span<const Node>(&cmd, 1));
                        if (!ok(ctx, t, r)) co_return;
                        break;
                    }
                }
            }
            co_return;
        }
    }
}

// Makes and spawns a script task whose body starts at `body` (a node of `a`'s command).
Expected<Node> start(Scheduler& s, Args& a, ScriptTask t, const Node* body) {
    Context& ctx = a.context();
    Context::Impl& impl = ctx.impl();
    const Array& cmd = a.command();
    t.command = Ref<Array>(const_cast<Array*>(&cmd));
    t.first = body ? static_cast<uint32_t>(body - cmd.begin()) : cmd.size();
    if (const Node* self = impl.cell_if(sym_this()); self && self->type() == Type::Object)
        t.self = *self;
    for (uint32_t i = t.first; i < cmd.size(); ++i)
        if (!capture(cmd[i], impl, t))
            return Unexpected(
                a.error("a task captures at most " + detail::decimal(kMaxCaptures) + " variables",
                        ErrorCode::Limit));
    const Symbol name = kind_name(t.kind);
    auto h = s.spawn(run(s, ctx, std::move(t)), name);
    if (!h) return Unexpected(a.error(h.error().message(), h.error().code()));
    return Node(static_cast<int64_t>(h->bits));
}

const Node* body_of(const Args& a, size_t i) {
    return i < a.size() ? &a.raw(i) : nullptr;
}

}  // namespace

void Scheduler::bind(Binder<Scheduler>& b) {
    b.method("delay_task", [](Scheduler& s, double t, Args body) -> Expected<Node> {
         ScriptTask task;
         task.kind = Kind::Delay;
         task.time = t;
         return start(s, body, std::move(task), body_of(body, 0));
     }).doc("`{scheduler delay_task t body...}`: runs `body` once, `t` units from now.");
    b.method("interp_task",
             [](Scheduler& s, Raw<Var> var, double from, double to, Args body) -> Expected<Node> {
                 if (var.is_property())
                     return Unexpected(body.error("interp_task steps a $variable, not a property"));
                 ScriptTask task;
                 task.kind = Kind::Interp;
                 task.var = var.name();
                 task.from = from;
                 task.to = to;
                 return start(s, body, std::move(task), body_of(body, 0));
             })
        .doc(
            "`{scheduler interp_task $var from to body...}`: steps `$var` from `from` to `to` "
            "over |to - from| units, running `body` each tick, the last time at exactly `to`.");
    b.method("thread_task",
             [](Scheduler& s, Args body) -> Expected<Node> {
                 ScriptTask task;
                 task.kind = Kind::Thread;
                 return start(s, body, std::move(task), body_of(body, 0));
             })
        .doc(
            "`{scheduler thread_task cmd...}`: runs its commands in order across ticks. "
            "`{wait cond}`, `{wait_for t}`, and `{wait_tick}` suspend it, as its own commands.");
    b.method("cancel", [](Scheduler& s, int64_t handle) {
         s.cancel(TaskHandle{static_cast<uint64_t>(handle)});
     }).doc("`{scheduler cancel handle}`: ends a task a *_task command returned.");
    b.method("now", [](Scheduler& s) {
         return s.now();
     }).doc("`{scheduler now}`: the Scheduler's time.");
}

void register_task_builtins(Context& ctx) {
    // The waits are thread_task's own forms; anywhere else they are an error.
    for (const char* name : {"wait", "wait_for", "wait_tick"}) {
        auto r = ctx.func(name, [](Args a) -> Expected<Node> {
            return Unexpected(
                a.error("{" + Str(a.name().str()) +
                            "} works only as a command of {scheduler thread_task ...}",
                        ErrorCode::Refused));
        });
        if (r) r->doc("Suspends a `{scheduler thread_task ...}`; only as one of its commands.");
    }
}

}  // namespace baker
