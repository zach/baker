// Context: dispatch, variables, overload resolution, and registration.
#include "internal.hpp"

#include <baker/parse.hpp>

#include "common/containers.hpp"
#include "data/text.hpp"

#include <cstdio>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace baker {
namespace {

thread_local Context* t_current = nullptr;

// Context::Impl::call's body, inlined where it is plain and where it is instrumented.
#if defined(_MSC_VER)
#define BAKER_INLINE __forceinline
#else
#define BAKER_INLINE [[gnu::always_inline]] inline
#endif

}  // namespace

// The stack is recorded where an error is made or first passes a call, with every frame
// still live; an error that has one keeps it as it unwinds further. Nothing on the
// successful path pays for it.
BAKER_COLD void record_stack(Error& e) {
    if (!e.stack().empty() || !t_current) return;
    for (const CallFrame* c = t_current->impl().top; c; c = c->parent)
        e.push_stack(t_current->stack_entry(*c));
}

BAKER_COLD Expected<Node> fail(const Array& at, ErrorCode code, std::string_view message) {
    Error e = make_error(code, message, at.file(), at.line());
    record_stack(e);
    return Unexpected(std::move(e));
}

namespace {

BAKER_COLD void locate(Error& e, const Array& at) {
    if (e.file().empty()) {
        e.set_location(at.file(), at.line());
    }
    record_stack(e);
}

BAKER_COLD Expected<Node> bad_head(const Array& command) {
    if (command.empty()) return fail(command, ErrorCode::Syntax, "empty command {}");
    return fail(command, ErrorCode::Type,
                "a command starts with a function or an object, not a " +
                    Str(type_name(command[0].type())));
}

BAKER_COLD Expected<Node> not_function(const Array& command, Symbol name) {
    return fail(command, ErrorCode::NotFound, quoted(name) + " is not a function");
}

BAKER_COLD Expected<Node> too_deep(const Array& command, uint32_t depth, uintptr_t used) {
    return fail(command, ErrorCode::Limit,
                "calls nested too deep (" + detail::decimal(depth) + " calls, " +
                    detail::decimal(static_cast<int64_t>(used / 1024)) + " KB of stack)");
}

BAKER_COLD Expected<Node> bad_arity(const Array& command, Symbol name, const detail::Overload& o,
                                    size_t n) {
    return fail(command, ErrorCode::Type, arity_message(name, o.min_args, o.max_args, n));
}

BAKER_COLD Expected<Node> no_overload(const Array& command, Symbol name,
                                      const Vec<detail::Overload>& overloads, const Node* values,
                                      size_t n) {
    Str given;
    for (size_t i = 0; i < n; ++i) {
        if (i) given += ' ';
        given += type_name(values[i].type());
    }
    Str have;
    for (const detail::Overload& o : overloads) have += " " + signature_text(o);
    return fail(command, ErrorCode::Type,
                "no overload of " + quoted(name) + " takes (" + given + "); it has" + have);
}

BAKER_COLD Expected<Node> not_object(const Array& command, const Node& head) {
    return fail(
        command, ErrorCode::Type,
        "a command starts with a function or an object, not a " + Str(type_name(head.type())));
}

BAKER_COLD Expected<Node> not_set(Symbol name) {
    return Unexpected(make_error(ErrorCode::NotFound, "$" + Str(name.str()) + " is not set"));
}

// Calls `o`, locating an error at the call. One return, so the result is never moved.
Expected<Node> invoke(const detail::Overload& o, Args& args, const Array& command) {
    Expected<Node> r = o.thunk(o.callable.get(), args);
    if (!r) locate(r.error(), command);
    return r;
}

// Evaluates a Command or Var for baker::data's typed lookups.
Expected<Node> evaluate_for_data(const Node& n) {
    if (!t_current)
        return Unexpected(make_error(ErrorCode::NotFound,
                                     "a " + Str(type_name(n.type())) +
                                         " value needs a current baker::Context on this thread"));
    return t_current->eval(n);
}

void stderr_sink(void*, const Error& notice) {
    const std::string_view text = describe(notice);
    std::fprintf(stderr, "%.*s\n", static_cast<int>(text.size()), text.data());
}

// The default trace sink: each call on entry, indented by depth, with where it is written;
// on exit its result or error.
void stderr_trace(void*, const TraceEvent& e) {
    Str line(2 * size_t{e.depth}, ' ');
    if (!e.result) {
        line += detail::preview(Node(Ref<Array>(const_cast<Array*>(e.command)), Type::Command), 60);
        if (!e.command->file().empty())
            line += "  (" + Str(e.command->file().str()) + ":" +
                    detail::decimal(e.command->line()) + ")";
    } else if (*e.result) {
        line += "-> " + detail::preview(**e.result, 60);
    } else {
        line += "-> error: " + Str(e.result->error().message());
    }
    std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data());
}

// Every counted call, as CallStats, ordered by self time, then calls.
Vec<CallStats> collect(const Context::Impl& impl, bool script_only, uint64_t threshold) {
    Vec<CallStats> out;
    auto add = [&](const Counters& c, Symbol name, Symbol on, Tier tier) {
        if (c.calls == 0 || (script_only && (tier != Tier::Script || c.self_ns < threshold)))
            return;
        out.push_back(CallStats{name, on, tier, c.calls, c.total_ns, c.self_ns});
    };
    for (const auto& f : impl.functions) add(f->counters, f->name, {}, f->tier);
    for (const auto& c : impl.classes)
        for (const ScriptClass::Handler& h : c->handlers)
            add(h.counters, h.name, c->name, Tier::Script);
    std::sort(out.begin(), out.end(), [](const CallStats& a, const CallStats& b) {
        return a.self_ns != b.self_ns ? a.self_ns > b.self_ns : a.calls > b.calls;
    });
    return out;
}

}  // namespace

// --- Signatures ---

Str signature_text(const detail::Overload& o) {
    Str out = "(";
    for (const detail::ParamInfo& p : o.params) {
        if (out.size() > 1) out += ' ';
        if (p.kind == detail::ParamKind::Optional) {
            out += '[';
            out += p.type();
            out += ']';
        } else {
            out += p.type();
        }
    }
    return out + ")";
}

Str arity_text(uint32_t min, uint32_t max) {
    Str n;
    if (max == UINT32_MAX)
        n = "at least " + detail::decimal(min);
    else if (min == max)
        n = detail::decimal(min);
    else
        n = detail::decimal(min) + " to " + detail::decimal(max);
    return n + (n == "1" || n == "at least 1" ? " argument" : " arguments");
}

Str arity_message(Symbol name, uint32_t min, uint32_t max, size_t given) {
    return quoted(name) + " takes " + arity_text(min, max) + ", given " +
           detail::decimal(static_cast<int64_t>(given));
}

// --- Impl ---

Node* Context::Impl::cell(Symbol name) {
    const uint32_t i = name.index();
    const uint32_t page = i >> kPageBits;
    if (page >= pages.size()) pages.resize(page + 1);
    if (!pages[page]) {
        constexpr size_t cells = size_t{1} << kPageBits;
        auto* p = static_cast<Node*>(detail::allocate(alloc, cells * sizeof(Node), alignof(Node)));
        for (size_t k = 0; k < cells; ++k) ::new (p + k) Node(kUnset);
        pages[page] = p;
    }
    return &pages[page][i & ((1u << kPageBits) - 1)];
}

Context::Impl::~Impl() {
    for (detail::TypeRecord* t : declared_types) detail::destroy(default_allocator(), t);
    constexpr size_t cells = size_t{1} << kPageBits;
    for (Node* p : pages) {
        if (!p) continue;
        for (size_t k = 0; k < cells; ++k) p[k].~Node();
        detail::deallocate(alloc, p, cells * sizeof(Node));
    }
}

Function& Context::Impl::function(Symbol name) {
    if (Function* f = find(name)) return *f;
    functions.push_back(Own<Function>::make(alloc, alloc));
    Function& f = *functions.back();
    f.name = name;
    if (name.index() >= by_index.size()) by_index.resize(name.index() + 1, nullptr);
    by_index[name.index()] = &f;
    return f;
}

Expected<Node> Context::Impl::eval(const Node& n) {
    switch (n.type()) {
        case Type::Command: return call(*n.array());
        case Type::Var: {
            const Node* c = cell_if(n.var_name());
            if (c && !is_unset(*c)) return *c;
            return not_set(n.var_name());
        }
        case Type::Property: return property_path(*n.array());
        default: return n;
    }
}

BAKER_INLINE Expected<Node> Context::Impl::call_body(const Array& command, Counters** counted) {
    if (command.empty()) return bad_head(command);
    // The head names a function, or is an object: named, an Object node, or a command or
    // variable that evaluates to one ("Object commands evaluate slot 0").
    const Node& head = command[0];
    const Function* f = nullptr;
    ObjectHandle target;
    Symbol name;
    if (head.type() == Type::Symbol) {
        name = *head.try_sym();
        f = find(name);
        if (!f || f->overload_count == 0) {
            f = nullptr;
            target = named(name);
            if (!target.bits) return not_function(command, name);
        }
    } else if (head.type() == Type::Object) {
        target = head.object_handle();
    } else if (head.type() == Type::Command || head.type() == Type::Var) {
        Expected<Node> r = eval(head);
        if (!r) {
            locate(r.error(), command);
            return r;
        }
        if (r->type() != Type::Object) return not_object(command, *r);
        target = r->object_handle();
    } else {
        return bad_head(command);
    }
    // Deep recursion is an error, not a crash: at most kMaxDepth calls, and at most
    // stack_budget bytes of stack below the outermost one (a debug build's frames are large).
    // The frame's address, not a local's: ASan can move address-taken locals to the heap.
#if defined(_MSC_VER)
    const auto at = reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());
#else
    const auto at = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
#endif
    if (depth == 0) stack_base = at;
    const uintptr_t used = at < stack_base ? stack_base - at : at - stack_base;
    if (depth >= kMaxDepth || used > stack_budget) return too_deep(command, depth, used);
    // This call joins the frame list for as long as it runs ("Debugging").
    struct Frame {
        Impl& impl;
        size_t kept;
        CallFrame call;
        ~Frame() {
            --impl.depth;
            impl.top = call.parent;
            if (impl.kept.size() > kept) impl.kept.resize(kept);
        }
    } frame{*this, kept.size(), {&command, top}};
    top = &frame.call;
    ++depth;
    if (!f) return send(target, command, counted);
    if constexpr (!kShipping) {
        ++f->counters.calls;
        if (counted) *counted = &f->counters;
    }
    if (f->overload_count > 1) return invoke_set(name, f->overloads, command, 1, nullptr);
    // One overload: the common case, kept inline, from the record's hot fields.
    const size_t n = command.size() - 1;
    if (n < f->min_args || n > f->max_args) return bad_arity(command, name, f->overloads[0], n);
    Args args(self, command, command.begin() + 1, n, 0, false);
    Expected<Node> r = f->thunk(f->callable, args);
    if (!r) locate(r.error(), command);
    return r;
}

Expected<Node> Context::Impl::call(const Array& command) {
    if constexpr (!kShipping)
        if (instrumented) return call_instrumented(command);
    return call_body(command, nullptr);
}

BAKER_COLD Expected<Node> Context::Impl::call_instrumented(const Array& command) {
    const uint32_t level = depth;
    if (tracing && trace_sink) trace_sink(trace_user, TraceEvent{&command, level, nullptr});
    Counters* counters = nullptr;
    uint64_t callees = 0;
    uint64_t* outer = child_ns;
    child_ns = &callees;
    const bool timed = profiling;
    const uint64_t start = timed ? clock(clock_user) : 0;
    Expected<Node> r = call_body(command, &counters);
    child_ns = outer;
    if (timed) {
        const uint64_t elapsed = clock(clock_user) - start;
        if (outer) *outer += elapsed;
        if (counters) {
            counters->total_ns += elapsed;
            counters->self_ns += elapsed > callees ? elapsed - callees : 0;
        }
    }
    if (tracing && trace_sink) trace_sink(trace_user, TraceEvent{&command, level, &r});
    return r;
}

// One overload is called directly, its arguments evaluated only as it reads them. An
// overload set evaluates every argument first, then picks the first overload whose
// parameter types fit: exactly, then with Int read as Float.
Expected<Node> Context::Impl::invoke_set(Symbol name, const Vec<detail::Overload>& overloads,
                                         const Array& command, size_t first, ObjectBase* object) {
    const size_t n = command.size() - first;
    if (overloads.size() == 1) {
        const detail::Overload& o = overloads[0];
        if (n < o.min_args || n > o.max_args) return bad_arity(command, name, o, n);
        Args args(self, command, command.begin() + first, n, 0, false, first, object);
        return invoke(o, args, command);
    }
    // Eight on the stack, more from the default allocator. Not a Vec: an empty std::vector
    // allocates in MSVC's debug library. Not a NodeBuffer: it costs 70 instructions a call.
    struct Spill {
        Node* p = nullptr;
        size_t n = 0;
        ~Spill() {
            for (size_t i = 0; i < n; ++i) p[i].~Node();
            if (p) detail::default_free(p, n * sizeof(Node));
        }
    } spill;
    Node small[8];
    Node* values = small;
    if (n > 8) {
        spill.p = static_cast<Node*>(detail::default_alloc(n * sizeof(Node), alignof(Node)));
        for (; spill.n < n; ++spill.n) ::new (spill.p + spill.n) Node();
        values = spill.p;
    }
    for (size_t i = 0; i < n; ++i) {
        auto r = eval(command[i + first]);
        if (!r) {
            locate(r.error(), command);
            return r;
        }
        values[i] = std::move(*r);
    }
    auto fits = [&](const detail::Overload& o, bool promote) {
        if (n < o.min_args || n > o.max_args) return false;
        for (size_t i = 0; i < n && i < o.params.size(); ++i) {
            const detail::ParamInfo& p = o.params[i];
            if (p.kind == detail::ParamKind::Rest) return true;
            if (p.match && !p.match(values[i], promote)) return false;
        }
        return true;
    };
    for (const bool promote : {false, true}) {
        for (const detail::Overload& o : overloads) {
            if (!fits(o, promote)) continue;
            Args args(self, command, values, n, 0, true, first, object);
            return invoke(o, args, command);
        }
    }
    return no_overload(command, name, overloads, values, n);
}

Expected<Node> Context::Impl::run(std::span<const Node> nodes) {
    Node last;
    for (const Node& n : nodes) {
        auto r = eval(n);
        if (!r) return r;
        last = std::move(*r);
    }
    return last;
}

// --- Context ---

const Allocator& Context::allocator() const noexcept {
    return impl_->alloc;
}

Context::Context(const Allocator& allocator)
    : impl_(detail::create<Impl>(allocator, *this, allocator)) {
    detail::set_evaluator(&evaluate_for_data);
    if (!t_current) t_current = this;
    impl_->sink = &stderr_sink;
    impl_->trace_sink = &stderr_trace;
    register_builtins(*this);
    register_object_builtins(*this);
    register_task_builtins(*this);
    // The default Scheduler, which scripts call `scheduler`.
    impl_->scheduler = Own<Scheduler>::make(impl_->alloc, *this);
    (void)add(*impl_->scheduler, "scheduler");
}

Context::~Context() {
    // Tasks first: their frames' destructors may still use the Context and its objects.
    impl_->scheduler = Own<Scheduler>();
    // Objects the Context created are deleted; the others just forget it.
    for (Impl::Slot& s : impl_->slots) {
        if (!s.object) continue;
        impl_->forget(*s.object);
        if (s.owned) impl_->destroy_owned(s.object);
    }
    if (t_current == this) t_current = nullptr;
    const Allocator alloc = impl_->alloc;
    detail::destroy(alloc, impl_);
}

Context::Scope::Scope(Context& ctx) noexcept : previous_(t_current) {
    t_current = &ctx;
}

Context::Scope::~Scope() {
    t_current = previous_;
}

Context* Context::current() noexcept {
    return t_current;
}

// The host's calls: what waited for the outermost call to return is settled as it does.
Expected<Node> Context::eval(const Node& node) {
    Scope scope(*this);
    Expected<Node> r = impl_->eval(node);
    if (impl_->unsettled && impl_->depth == 0) impl_->settle();
    return r;
}

Expected<Node> Context::run(std::span<const Node> nodes) {
    Scope scope(*this);
    Expected<Node> r = impl_->run(nodes);
    if (impl_->unsettled && impl_->depth == 0) impl_->settle();
    return r;
}

List<Error> Context::run_autorun(std::span<const Node> commands) {
    impl_->retract_autorun();
    impl_->in_autorun = true;
    Vec<Error> failed;
    for (const Node& command : commands)
        if (auto r = eval(command); !r) failed.push_back(std::move(r).error());
    impl_->in_autorun = false;
    return failed.empty() ? List<Error>() : List<Error>({failed.data(), failed.size()});
}

Expected<Node> Context::eval(std::string_view text, std::string_view name) {
    LoadSession session;
    session.allocator = &impl_->alloc;  // the tree is the Context's
    auto tree = read_string(text, name, session);
    if (!tree) return Unexpected(std::move(tree).error());
    return run((*tree)->nodes());
}

bool Context::has_func(Symbol name) const noexcept {
    const Function* f = impl_->find(name);
    return f && !f->overloads.empty();
}

VarRef Context::var(Symbol name) {
    return VarRef(impl_->cell(name), name);
}

VarRef Context::global(Symbol name, Node initial, std::string_view doc) {
    VarRef v = var(name);
    v = std::move(initial);
    for (Global& g : impl_->globals)
        if (g.name == name) {
            g.doc = doc;
            return v;
        }
    impl_->globals.push_back({name, Str(doc)});
    return v;
}

void Context::set_stack_budget(size_t bytes) noexcept {
    impl_->stack_budget = bytes;
}

// --- Debugging ---

const CallFrame* Context::top_frame() const noexcept {
    return impl_->top;
}

StackEntry Context::stack_entry(const CallFrame& frame) const {
    const Array& c = *frame.command;
    StackEntry e{{}, {}, c.file(), c.line()};
    if (c.empty()) return e;
    const auto head = c[0].try_sym();
    if (head && has_func(*head)) {
        e.function = *head;
        return e;
    }
    // An object command: its method, and the object's name if it has one.
    if (c.size() > 1) e.function = c[1].try_sym().value_or(Symbol());
    if (head) {
        e.on = *head;
    } else if (c[0].type() == Type::Object) {
        if (const Impl::Slot* s = impl_->slot(c[0].object_handle())) e.on = s->name;
    }
    return e;
}

List<StackEntry> Context::backtrace() const {
    Vec<StackEntry> out;
    for (const CallFrame* c = impl_->top; c; c = c->parent) out.push_back(stack_entry(*c));
    return List<StackEntry>(out);
}

void script_backtrace(TextSink out) {
    if (const Context* ctx = Context::current())
        for (const CallFrame* c = ctx->top_frame(); c; c = c->parent) {
            out("in ");
            write(ctx->stack_entry(*c), out);
            out("\n");
        }
}

void Context::set_profiling(bool on) noexcept {
    if constexpr (kShipping) return;
    impl_->profiling = on;
    impl_->instrumented = impl_->profiling || impl_->tracing;
}

void Context::set_time_source(TimeSource fn, void* user) noexcept {
    impl_->clock = fn ? fn : steady_now;
    impl_->clock_user = user;
}

List<CallStats> Context::stats() const {
    if constexpr (kShipping) return {};
    const Vec<CallStats> all = collect(*impl_, false, 0);
    return List<CallStats>(std::span<const CallStats>(all.data(), all.size()));
}

void Context::reset_stats() noexcept {
    for (const auto& f : impl_->functions) f->counters = {};
    for (const auto& c : impl_->classes)
        for (const ScriptClass::Handler& h : c->handlers) h.counters = {};
}

List<CallStats> Context::hardening_candidates(uint64_t threshold_ns) const {
    if constexpr (kShipping) return {};
    const Vec<CallStats> found = collect(*impl_, true, threshold_ns);
    return List<CallStats>(std::span<const CallStats>(found.data(), found.size()));
}

void Context::trace(bool on) noexcept {
    if constexpr (kShipping) return;
    impl_->tracing = on;
    impl_->instrumented = impl_->profiling || impl_->tracing;
}

void Context::set_trace_sink(TraceSink sink, void* user) noexcept {
    impl_->trace_sink = sink;
    impl_->trace_user = user;
}

void Context::set_notice_sink(NoticeSink sink, void* user) noexcept {
    impl_->sink = sink;
    impl_->sink_user = user;
}

void Context::notice(const Error& notice) const {
    if (impl_->sink) impl_->sink(impl_->sink_user, notice);
}

Expected<FuncRef> Context::add_overload(Symbol name, detail::Overload o) {
    auto has_raw = [](const detail::Overload& x) {
        for (const detail::ParamInfo& p : x.params)
            if (p.raw) return true;
        return false;
    };
    Function& f = impl_->function(name);
    if (f.tier == Tier::Script && !f.overloads.empty()) {
        const uint32_t n = f.overloads[0].min_args;
        Error e = make_error(ErrorCode::Refused,
                             "native " + quoted(name) + " replaces the script definition", f.file,
                             f.line);
        if (n < o.min_args || n > o.max_args)
            e.set_message(Str(e.message()) + "; the script took " + arity_text(n, n) +
                          ", the native takes " + arity_text(o.min_args, o.max_args));
        notice(e);
        impl_->retire(f.overloads);
        f.refresh();
        f.status = Status::Normal;
        f.doc.clear();
    }
    for (const detail::Overload& x : f.overloads) {
        if (has_raw(x) || has_raw(o))
            return Unexpected(make_error(
                ErrorCode::Refused, quoted(name) + " takes unevaluated arguments, so it cannot be "
                                                   "overloaded"));
        if (x.min_args == o.min_args && x.max_args == o.max_args &&
            signature_text(x) == signature_text(o))
            return Unexpected(
                make_error(ErrorCode::Refused,
                           quoted(name) + " already has the overload " + signature_text(o)));
    }
    f.tier = Tier::Native;
    f.overloads.push_back(std::move(o));
    f.refresh();
    return FuncRef(&f);
}

// --- Functions declared from a description ---

namespace {

// A declared function's callable: its name, for the error a call gets, and the parameter
// table its overload's params point into, with the type names it owns.
struct Declared {
    // `infos`: each parameter's kind, match, and raw, as checked; its type is `od`'s.
    Declared(const Allocator& a, Symbol n, const OverloadDescription& od,
             const detail::ParamInfo* infos, Callback<Expected<Node>(Args&)> b)
        : name(n), params(a), names(a), values(a), texts(a), result(a), body(std::move(b)) {
        for (size_t i = 0; i < od.params.size(); ++i) raw = raw || infos[i].raw;
        result = od.result;
        // A parameter's value types: the description's, or a type of Baker's own, as the
        // game's native parameter has them.
        auto given = [](const ParamDescription& p) {
            return !p.values.empty() || !detail::value_type_known(p.type)
                       ? p.values
                       : std::span<const std::string_view>(&p.type, 1);
        };
        size_t length = 0, count = 0;
        for (const ParamDescription& p : od.params) {
            length += p.type.size();
            for (const std::string_view v : given(p)) length += v.size();
            count += given(p).size();
        }
        names.reserve(length);  // so the views below stay where they point
        values.reserve(count);
        texts.reserve(od.params.size());
        for (size_t i = 0; i < od.params.size(); ++i) {
            const ParamDescription& p = od.params[i];
            const std::string_view type = keep(p.type);
            const size_t first = values.size();
            for (const std::string_view v : given(p)) values.push_back(keep(v));
            texts.push_back({type, {values.data() + first, given(p).size()}});
            params.push_back(infos[i]);
            params.back().text = &texts.back();
        }
    }
    std::string_view keep(std::string_view text) {
        const size_t at = names.size();
        names += text;
        return names.view().substr(at, text.size());
    }
    Symbol name;
    Vec<detail::ParamInfo> params;
    Str names;                     // every type name, one after the other
    Vec<std::string_view> values;  // every parameter's value types, one after the other
    Vec<detail::ParamText> texts;  // what params point to
    Str result;
    Callback<Expected<Node>(Args&)> body;  // none: declared only, for check
    bool raw = false;                      // a parameter is passed as written
};

// Whether an argument has a described parameter's type: by its match, or by the value types
// of a type only described, read as Values::read reads a compound value.
bool has_type(const detail::ParamInfo& p, const Node& v) {
    if (p.match) return p.match(v, true);
    if (p.values().empty()) return true;
    const Array* a = v.try_array();
    return detail::reads_as(
        p.values(), a ? Values(a->nodes(), a) : Values(std::span<const Node>(&v, 1), nullptr));
}

Expected<Node> call_declared(const void* callable, Args& args) {
    const auto& d = *static_cast<const Declared*>(callable);
    if (!d.body)
        return Unexpected(make_error(ErrorCode::Refused, quoted(d.name) +
                                                             " was declared from a description, "
                                                             "and has no body to run"));
    if (d.raw) return d.body(args);
    // Each argument evaluated once and held to its parameter, as a native's: eight on the
    // stack, more from the default allocator (not a Vec: see Impl::call).
    const size_t n = args.size();
    struct Spill {
        Node* p = nullptr;
        size_t n = 0;
        ~Spill() {
            for (size_t i = 0; i < n; ++i) p[i].~Node();
            if (p) detail::default_free(p, n * sizeof(Node));
        }
    } spill;
    Node small[8];
    Node* values = small;
    if (n > 8) {
        spill.p = static_cast<Node*>(detail::default_alloc(n * sizeof(Node), alignof(Node)));
        for (; spill.n < n; ++spill.n) ::new (spill.p + spill.n) Node();
        values = spill.p;
    }
    for (size_t i = 0; i < n; ++i) {
        auto v = args.eval(i);
        if (!v) return v;
        const detail::ParamInfo* p = i < d.params.size() ? &d.params[i] : nullptr;
        if (p && p->kind != detail::ParamKind::Rest && !has_type(*p, *v))
            return Unexpected(args.error("argument " +
                                         detail::decimal(static_cast<int64_t>(i + 1)) + " of " +
                                         quoted(args.name()) + " is " + Str(type_name(v->type())) +
                                         ", not " + Str(p->type())));
        values[i] = std::move(*v);
    }
    Args evaluated(args.context(), args.command(), values, n, 0, true, args.command().size() - n,
                   args.self());
    return d.body(evaluated);
}

using Match = bool (*)(const Node&, bool) noexcept;

// `name`: a path or a layer name, a symbol or a string, as the config tools read one.
bool match_name(const Node& n, bool) noexcept {
    return n.type() == Type::Symbol || n.type() == Type::String;
}

// Baker's own type names, as the reference writes them for its functions (the config
// tools' included), and how each checks an argument.
bool builtin_match(std::string_view type, Match& match) {
    struct Entry {
        std::string_view name;
        Match match;
    };
    static constexpr Entry kTypes[] = {
        {"value", &detail::match_value<Node>},
        {"unevaluated", nullptr},
        {"values...", nullptr},
        {"$variable", detail::Param<Raw<Var>>::match},
        {"array", detail::Param<const Array&>::match},
        {"symbol", &detail::match_value<Symbol>},
        {"string", &detail::match_value<std::string_view>},
        {"name", &match_name},
        {"bool", &detail::match_value<bool>},
        {"int", &detail::match_value<int64_t>},
        {"int8", &detail::match_value<int64_t>},
        {"int16", &detail::match_value<int64_t>},
        {"int32", &detail::match_value<int64_t>},
        {"uint8", &detail::match_value<int64_t>},
        {"uint16", &detail::match_value<int64_t>},
        {"uint32", &detail::match_value<int64_t>},
        {"uint64", &detail::match_value<int64_t>},
        {"float", &detail::match_value<double>},
        {"float32", &detail::match_value<double>},
    };
    for (const Entry& e : kTypes)
        if (e.name == type) {
            match = e.match;
            return true;
        }
    return false;
}

// A type of the game's own the description gives the value types of: checked by them.
bool described(const ParamDescription& p) {
    return !p.values.empty() && std::all_of(p.values.begin(), p.values.end(),
                                            [](auto v) { return detail::value_type_known(v); });
}

// A type a native function or method of this Context takes (a host's own, `vec2`), with
// the check its parameter makes.
bool registered_match(const Context::Impl& impl, std::string_view type, Match& match) {
    auto in = [&](const Vec<detail::Overload>& overloads) {
        for (const detail::Overload& o : overloads)
            for (const detail::ParamInfo& p : o.params)
                if (p.type() == type && p.match) {
                    match = p.match;
                    return true;
                }
        return false;
    };
    for (const auto& f : impl.functions)
        if (f->tier == Tier::Native && in(f->overloads)) return true;
    for (const detail::TypeRecord* t : impl.types)
        for (const detail::MethodEntry& m : t->methods)
            if (in(m.overloads)) return true;
    return false;
}

// Builds each overload of `name` from its description into `built`; why not, if one
// cannot be, and then `built` is not to be added.
std::optional<Str> build_described(Context::Impl& impl, Symbol name,
                                   std::span<const OverloadDescription> overloads,
                                   Vec<detail::Overload>& built,
                                   const Callback<Expected<Node>(Args&)>& body = {}) {
    if (overloads.empty()) return Str("it has no overloads");
    Vec<detail::ParamInfo> infos(impl.alloc);
    for (const OverloadDescription& od : overloads) {
        detail::Overload o;
        infos.clear();
        for (const ParamDescription& p : od.params) {
            detail::ParamInfo info{};
            if (o.max_args == UINT32_MAX) return Str("a parameter follows the rest parameter");
            if (p.kind == "value")
                info.kind = detail::ParamKind::Value;
            else if (p.kind == "raw")
                info.kind = detail::ParamKind::Raw;
            else if (p.kind == "optional")
                info.kind = detail::ParamKind::Optional;
            else if (p.kind == "rest")
                info.kind = detail::ParamKind::Rest;
            else
                return "a parameter's kind is \"" + Str(p.kind) +
                       "\", not value, raw, optional, or rest";
            if (!builtin_match(p.type, info.match) && !registered_match(impl, p.type, info.match) &&
                !described(p))
                return "this Context has no type \"" + Str(p.type) +
                       "\", and its description gives no value types of Baker's";
            info.raw = p.raw || info.kind == detail::ParamKind::Raw;
            infos.push_back(info);
            if (info.kind == detail::ParamKind::Rest) {
                o.max_args = UINT32_MAX;
            } else {
                ++o.max_args;
                if (info.kind != detail::ParamKind::Optional) o.min_args = o.max_args;
            }
        }
        o.callable =
            detail::Shared::make_in<Declared>(impl.alloc, impl.alloc, name, od, infos.data(), body);
        const auto& d = *static_cast<const Declared*>(o.callable.get());
        o.thunk = &call_declared;
        o.params = std::span<const detail::ParamInfo>(d.params.data(), d.params.size());
        o.result = d.result.view();
        built.push_back(std::move(o));
    }
    return std::nullopt;
}

}  // namespace

Expected<FuncRef> Context::declare(Symbol name, std::span<const OverloadDescription> overloads) {
    return declare(name, overloads, {});
}

Expected<FuncRef> Context::declare(Symbol name, std::span<const OverloadDescription> overloads,
                                   const Callback<Expected<Node>(Args&)>& body) {
    auto refuse = [&](std::string_view why) {
        return Unexpected(
            make_error(ErrorCode::Type, "cannot declare " + quoted(name) + ": " + Str(why)));
    };
    // Every overload is built before any is added, so a description with a mistake adds
    // nothing.
    Vec<detail::Overload> built(impl_->alloc);
    if (auto why = build_described(*impl_, name, overloads, built, body)) return refuse(*why);
    Expected<FuncRef> ref = refuse("unreachable");
    for (detail::Overload& o : built)
        if (ref = add_overload(name, std::move(o)); !ref) return ref;
    return ref;
}

Expected<void> Context::declare_type(Symbol name, std::span<const Symbol> bases,
                                     std::span<const MethodDescription> methods) {
    auto refuse = [&](std::string_view why) {
        return Unexpected(
            make_error(ErrorCode::Type, "cannot declare type " + quoted(name) + ": " + Str(why)));
    };
    if (name.empty()) return refuse("it has no name");
    if (impl_->type_named(name))
        return Unexpected(make_error(ErrorCode::Refused, "cannot declare type " + quoted(name) +
                                                             ": this Context already has a "
                                                             "type of that name"));
    Vec<const detail::TypeRecord*> found(impl_->alloc);
    for (const Symbol b : bases) {
        const detail::TypeRecord* t = impl_->type_named(b);
        if (!t) return refuse("its base " + quoted(b) + " is not a type this Context knows");
        found.push_back(t);
    }
    Vec<Vec<detail::Overload>> built(impl_->alloc);
    for (const MethodDescription& m : methods) {
        built.emplace_back(impl_->alloc);
        if (auto why = build_described(*impl_, m.name, m.overloads, built.back()))
            return refuse("method " + quoted(m.name) + ": " + *why);
    }
    detail::TypeRecord* type = detail::new_type_record(name.str());
    for (const detail::TypeRecord* b : found) detail::inherit(*type, *b);
    for (size_t i = 0; i < methods.size(); ++i)
        for (detail::Overload& o : built[i])
            (void)detail::add_method(*type, methods[i].name, std::move(o));
    type->sort();
    impl_->declared_types.push_back(type);
    add_type(*type);
    return {};
}

bool Context::has_type(Symbol name) const noexcept {
    return impl_->type_named(name) != nullptr;
}

Expected<void> Context::declare_object(Symbol name, Symbol type) {
    auto refuse = [&](std::string_view why) {
        return Unexpected(
            make_error(ErrorCode::Type, "cannot declare object " + quoted(name) + ": " + Str(why)));
    };
    if (!impl_->type_named(type))
        return refuse("its type " + quoted(type) + " is not a type this Context knows");
    if (impl_->find(name) || impl_->slot(impl_->named(name)) ||
        !impl_->declared_object(name).empty())
        return Unexpected(make_error(ErrorCode::Refused, "cannot declare object " + quoted(name) +
                                                             ": it is already a function or an "
                                                             "object"));
    impl_->declared_objects.emplace_back(name, type);
    return {};
}

FuncRef& FuncRef::doc(std::string_view text) {
    static_cast<Function*>(f_)->doc = text;
    return *this;
}

FuncRef& FuncRef::status(Status status, Symbol replacement) {
    auto* f = static_cast<Function*>(f_);
    f->status = status;
    f->replacement = replacement;
    return *this;
}

// --- Args, Raw, VarRef ---

Expected<Node> Args::eval_node(const Node& n) const {
    Expected<Node> r = ctx_->impl().eval(n);
    if (!r) locate(r.error(), *command_);
    return r;
}

Expected<Node> Args::eval(size_t i) const {
    if (i >= size_) return Unexpected(type_error(i, Node(), "value"));
    if (evaluated_) return base_[i];
    return eval_node(base_[i]);
}

Expected<const Array*> Args::array(size_t i) const {
    // A literal array, or a value an overload set evaluated, is held for the whole call by
    // the command or by `kept`: no copy to keep.
    if (i < size_ && (evaluated_ || base_[i].type() == Type::Array))
        if (const Array* a = base_[i].type() == Type::Array ? base_[i].try_array() : nullptr)
            return a;
    auto r = eval(i);
    if (!r) return Unexpected(std::move(r).error());
    if (r->type() != Type::Array) return Unexpected(type_error(i, *r, "array"));
    const Array* a = r->try_array();
    keep(std::move(*r));
    return a;
}

Args Args::rest(size_t i) const noexcept {
    const size_t k = i < size_ ? i : size_;
    return Args(*ctx_, *command_, base_ + k, size_ - k, offset_ + k, evaluated_, first_, self_);
}

Symbol Args::name() const noexcept {
    const Node* n = command_->try_at(first_ - 1u);
    return n ? n->try_sym().value_or(Symbol()) : Symbol();
}

Error Args::error(std::string_view message, ErrorCode code) const {
    return make_error(code, message, command_->file(), command_->line());
}

Error Args::type_error(size_t i, const Node& n, std::string_view want) const {
    const Str which = "argument " + detail::decimal(static_cast<int64_t>(offset_ + i + 1)) +
                      " of " + quoted(name());
    if (i >= size_)
        return error(quoted(name()) + " is missing " + which.substr(0, which.find(" of")));
    return error(which + " is " + Str(type_name(n.type())) + ", not " + Str(want));
}

void Args::keep(Node n) const {
    ctx_->impl().kept.push_back(std::move(n));
}

namespace detail {
Expected<Raw<Var>> read_var(const Args& a, size_t i) {
    const Node& n = a.raw(i);
    if (n.type() == Type::Property) return Raw<Var>(a.context(), *n.array());
    if (n.type() != Type::Var)
        return Unexpected(a.error("argument " + detail::decimal(static_cast<int64_t>(i + 1)) +
                                  " of " + quoted(a.name()) + " must be a $variable, not " +
                                  Str(type_name(n.type()))));
    return Raw<Var>(a.context().impl().cell(n.var_name()), n.var_name());
}
}  // namespace detail

Expected<Node> Raw<Node>::eval() const {
    return ctx_->impl().eval(*node_);
}

bool VarRef::is_set() const noexcept {
    return !is_unset(*cell_);
}

const Node& VarRef::value() const noexcept {
    static const Node kZero;
    return is_unset(*cell_) ? kZero : *cell_;
}

void VarRef::unset() noexcept {
    *cell_ = kUnset;
}

}  // namespace baker
