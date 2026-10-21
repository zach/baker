#pragma once

// The inside of a Context, shared by the files of baker::script.

#include <baker/object.hpp>
#include <baker/script.hpp>
#include <baker/task.hpp>

#include "common/containers.hpp"
#include "data/text.hpp"

#include <algorithm>
#include <utility>

namespace baker {

namespace detail {

struct MethodEntry {
    Symbol name;
    Vec<Overload> overloads;
    Str doc;
    Status status = Status::Normal;
    Symbol replacement;
    bool inherited = false;
    std::string_view message;  // the message type this method is the script face of
};

// A typed message handler (Binder::on), searched by key on the C++ path.
struct MessageEntry {
    const void* key;
    MessageThunk thunk;
    Shared callable;
};

struct PropertyEntry {
    Symbol name;
    std::string_view type;
    Shared accessor;
    PropertyGet get = nullptr;
    PropertySet set = nullptr;
    Str doc;
};

// An object's loose parameters: the Context's, from its allocator, for as long as the
// object is in it.
struct ObjectParams {
    explicit ObjectParams(const Allocator& a) : items(a) {}
    Vec<std::pair<Symbol, Node>> items;
};

struct TypeRecord {
    Str name;
    Symbol symbol;                 // `name`, interned
    Vec<const TypeRecord*> bases;  // every ancestor, nearest first
    Vec<MethodEntry> methods;      // sorted by symbol identity, for binary search
    Vec<MessageEntry> messages;    // sorted by key; a type has few
    Vec<PropertyEntry> properties;
    CreateObject create = nullptr;  // for {new}; null if T is not default-constructible
    DestroyObject destroy = nullptr;
    Str error;  // a binding mistake, reported when T is first added

    [[nodiscard]] bool is(const TypeRecord* t) const noexcept {
        return t == this || std::find(bases.begin(), bases.end(), t) != bases.end();
    }
    [[nodiscard]] const MethodEntry* method(Symbol name) const noexcept;
    [[nodiscard]] const PropertyEntry* property(Symbol name) const noexcept;
    void sort();
};

}  // namespace detail

// A call statistics record, updated in place. Mutable: counting a call does not
// change what is called.
struct Counters {
    uint64_t calls = 0;
    uint64_t total_ns = 0;
    uint64_t self_ns = 0;
};

struct Function : FuncRecord {
    explicit Function(const Allocator& a) : doc(a), overloads(a) {}

    // Hot: what a call reads, first, beside the name. A function with one overload
    // is called from these without touching the overload list.
    detail::Overload::Thunk thunk = nullptr;  // the only overload's; null for none or a set
    const void* callable = nullptr;
    uint32_t min_args = 0, max_args = 0;
    uint32_t overload_count = 0;
    mutable Counters counters;

    // Cold.
    Tier tier = Tier::Native;
    Status status = Status::Normal;
    Symbol replacement;  // for Deprecated
    Str doc;
    Vec<detail::Overload> overloads;
    Symbol file;  // where a script function is defined
    int32_t line = 0;

    // Copies the hot fields from `overloads`: after every change to it.
    void refresh() noexcept {
        overload_count = static_cast<uint32_t>(overloads.size());
        const bool one = overloads.size() == 1;
        thunk = one ? overloads[0].thunk : nullptr;
        callable = one ? overloads[0].callable.get() : nullptr;
        min_args = one ? overloads[0].min_args : 0;
        max_args = one ? overloads[0].max_args : 0;
    }
};

// A {func} body: run with the parameters bound.
struct ScriptBody {
    explicit ScriptBody(const Allocator& a = default_allocator()) : params(a), param_info(a) {}
    Vec<Symbol> params;
    Vec<detail::ParamInfo> param_info;  // one "value" per parameter, for Overload
    Ref<Array> definition;
    size_t body = 0;  // index of the first body node in `definition`
};

// A {class}: script handlers for a native type or a script-only class.
struct ScriptClass {
    struct Handler {
        Symbol name;
        ScriptBody body;
        mutable Counters counters;
    };
    explicit ScriptClass(const Allocator& a) : handlers(a) {}
    Symbol name;
    Vec<Handler> handlers;  // from the Context's allocator, as the class

    [[nodiscard]] const Handler* handler(Symbol method) const noexcept {
        for (const Handler& h : handlers)
            if (h.name == method) return &h;
        return nullptr;
    }
};

struct Global {
    Symbol name;
    Str doc;
};

// "(int float)", as the reference and messages write a signature.
[[nodiscard]] Str signature_text(const detail::Overload& o);
// "2 arguments", "1 to 2 arguments", "at least 1 argument".
[[nodiscard]] Str arity_text(uint32_t min, uint32_t max);
// "'name' takes 2 arguments, given 3": every arity error, at a call or in a check.
[[nodiscard]] Str arity_message(Symbol name, uint32_t min, uint32_t max, size_t given);
using detail::quoted;

// Errors are built out of line, so the evaluator's frames, which nest once per script call,
// hold no message temporaries.
#if defined(_MSC_VER)
#define BAKER_COLD __declspec(noinline)
#else
#define BAKER_COLD [[gnu::noinline, gnu::cold]]
#endif

// Records the current Context's live call frames in `e`, unless it already has a stack.
void record_stack(Error& e);
// An error located at `at`, with the call stack.
BAKER_COLD Expected<Node> fail(const Array& at, ErrorCode code, std::string_view message);

// A variable cell never set holds a Func node with no record, which nothing else makes.
inline const Node kUnset = Node::func(nullptr);
[[nodiscard]] inline bool is_unset(const Node& n) noexcept {
    return n.type() == Type::Func && !n.func_record();
}
// $this, bound in handlers and the tasks they make.
[[nodiscard]] inline const Symbol& sym_this() {
    static const Symbol s("this");
    return s;
}
// Registers the functions every Context starts with (builtins.cpp, object.cpp).
void register_builtins(Context& ctx);
void register_object_builtins(Context& ctx);
void register_task_builtins(Context& ctx);  // {wait}, {wait_for}, {wait_tick} (script_tasks.cpp)
// Parses a {func}-style parameter list and body from a definition array, starting at
// `params` (the index of the ($a ...) list).
[[nodiscard]] bool parse_params(const Array& params, ScriptBody& body);

struct Context::Impl {
    static constexpr uint32_t kPageBits = 10;
    static constexpr uint32_t kMaxDepth = 1000;

    Impl(Context& c, const Allocator& a) : alloc(a), self(c) {}
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] Function* find(Symbol name) const noexcept {
        const uint32_t i = name.index();
        return i < by_index.size() ? by_index[i] : nullptr;
    }
    [[nodiscard]] const Node* cell_if(Symbol name) const noexcept {
        const uint32_t i = name.index();
        const uint32_t page = i >> kPageBits;
        return page < pages.size() && pages[page] ? &pages[page][i & ((1u << kPageBits) - 1)]
                                                  : nullptr;
    }
    [[nodiscard]] Node* cell(Symbol name);
    [[nodiscard]] Function& function(Symbol name);

    [[nodiscard]] Expected<Node> eval(const Node& n);
    [[nodiscard]] Expected<Node> call(const Array& command);
    // The call itself. `counted`, if given, receives the counters of what was called.
    [[nodiscard]] Expected<Node> call_body(const Array& command, Counters** counted);
    // A call while profiling or tracing: times it, and reports it to the trace sink.
    [[nodiscard]] Expected<Node> call_instrumented(const Array& command);
    // Calls the overload set `overloads` of `name` with the arguments of `command` from
    // index `first` (2 for a method, whose object is `self`).
    [[nodiscard]] Expected<Node> invoke_set(Symbol name, const Vec<detail::Overload>& overloads,
                                            const Array& command, size_t first, ObjectBase* self);
    [[nodiscard]] Expected<Node> run(std::span<const Node> nodes);
    // Runs a script function or handler: arguments evaluated, then bound with $this.
    [[nodiscard]] Expected<Node> run_script(const ScriptBody& body, Args& args,
                                            const Node* this_node);

    // --- Objects (object.cpp). ---
    // Pointers first, then the small fields: 32 bytes, two to a cache line.
    struct Slot {
        ObjectBase* object = nullptr;
        ScriptClass* script = nullptr;  // the class of a script-only object
        Symbol name;
        uint32_t salt = 1;
        bool owned = false;  // created by {new}: the Context deletes it
    };
    static_assert(sizeof(Slot) == 4 * sizeof(void*), "Slot: two to a 64-byte cache line");
    [[nodiscard]] Slot* slot(ObjectHandle h) noexcept {
        const uint32_t i = h.index();
        return i < slots.size() && slots[i].object && slots[i].salt == h.salt() ? &slots[i]
                                                                                : nullptr;
    }
    [[nodiscard]] ObjectHandle named(Symbol name) const noexcept {
        const uint32_t i = name.index();
        return i < by_name.size() ? by_name[i] : ObjectHandle{};
    }
    [[nodiscard]] Expected<Node> add(ObjectBase& object, Symbol name, ScriptClass* script,
                                     bool owned);
    void forget(ObjectBase& o) noexcept;
    void remove(ObjectHandle h) noexcept;
    // A removal made during a tick, applied at the sync point.
    void defer_removal(ObjectHandle h);
    void remove_now(ObjectHandle h) noexcept;
    // Storage a redefinition replaces: kept while a call may still be running from it.
    void retire(Vec<detail::Overload>& overloads);
    void retire(Vec<ScriptClass::Handler>& handlers);
    // Called when the host's outermost eval or run has returned, if `unsettled`.
    void settle();
    // While run_autorun runs a layer's commands: records a function's or a class's state
    // before its first change, so that the next run_autorun can restore it.
    void note_function(Symbol name);
    void note_class(Symbol name);
    // Restores what the last run_autorun's commands changed, newest first.
    void retract_autorun();
    // Deletes an object {new} made: its type knows its size.
    void destroy_owned(ObjectBase* object) noexcept { object->type().destroy(alloc, object); }
    [[nodiscard]] Expected<Node> send(ObjectHandle target, const Array& command,
                                      Counters** counted = nullptr);
    [[nodiscard]] Expected<Node> property_path(const Array& path);
    [[nodiscard]] Expected<void> set_property(ObjectBase& object, Symbol name, const Node& value,
                                              const Array& at);
    [[nodiscard]] ScriptClass* script_class(Symbol name) const noexcept;
    // The script handler `method` sent to the object in `s` reaches, if any.
    [[nodiscard]] const ScriptClass::Handler* handler_for(const Slot& s, Symbol method) const;
    [[nodiscard]] const detail::TypeRecord* type_named(Symbol name) const noexcept;
    // The name a type goes by in messages: a script class's, else the native type's.
    [[nodiscard]] Str class_name(const Slot& s) const;

    // Binds `name` to `value` until the innermost Rebind ends.
    void bind(Symbol name, Node value) {
        Node* c = cell(name);
        saved.emplace_back(c, std::move(*c));
        *c = std::move(value);
    }

    const Allocator alloc;  // first: the containers below take it
    Context& self;
    Vec<Function*> by_index{alloc};  // by Symbol::index()
    Vec<Own<Function>> functions{alloc};
    // Variable cells, by Symbol::index(): pages of 1 << kPageBits, owned. Plain pointers, as
    // they were before Baker's own containers: through a wrapper, GCC stopped merging
    // the stores that copy a cell, and a read of $x lost 4 ns to store forwarding.
    Vec<Node*> pages{alloc};
    Vec<std::pair<Node*, Node>> saved{alloc};  // values shadowed by bind()
    Vec<Node> kept{alloc};                     // evaluated values a call refers to
    Vec<Global> globals{alloc};
    Vec<Slot> slots{alloc};
    Vec<uint32_t> free_slots{alloc};
    Vec<ObjectHandle> by_name{alloc};  // by Symbol::index()
    Vec<const detail::TypeRecord*> types{alloc};
    // Types and objects a tool declared from the reference, for check(): the types are the
    // Context's to free; an object is a name and its type.
    Vec<detail::TypeRecord*> declared_types{alloc};
    Vec<std::pair<Symbol, Symbol>> declared_objects{alloc};
    [[nodiscard]] Symbol declared_object(Symbol name) const noexcept {
        for (const auto& [n, type] : declared_objects)
            if (n == name) return type;
        return {};
    }
    Vec<Own<ScriptClass>> classes{alloc};
    Vec<ScriptClass*> class_by_name{alloc};  // by Symbol::index()
    // Bit per Symbol::index(): some {class} has a handler of that name. The usual answer
    // for a message, "none", costs one load.
    Vec<uint64_t> handled_names{alloc};
    [[nodiscard]] bool handled(Symbol name) const noexcept {
        const uint32_t i = name.index();
        return i / 64 < handled_names.size() && (handled_names[i / 64] >> (i % 64) & 1);
    }
    const CallFrame* top = nullptr;  // the innermost call in progress
    uint32_t depth = 0;
    uintptr_t stack_base = 0;                        // the outermost call's stack address
    uintptr_t stack_budget = uintptr_t{256} * 1024;  // see Context::set_stack_budget
    NoticeSink sink = nullptr;
    void* sink_user = nullptr;

    // Statistics and tracing (not under BAKER_SHIPPING).
    bool profiling = false;
    bool tracing = false;
    bool instrumented = false;  // profiling || tracing: the one test on the call path
    TimeSource clock = steady_now;
    void* clock_user = nullptr;
    uint64_t* child_ns = nullptr;  // the innermost timed call's time spent in its callees
    Context::TraceSink trace_sink = nullptr;
    void* trace_user = nullptr;

    Own<Scheduler> scheduler;  // the default one, the object `scheduler`
    uint32_t ticking = 0;      // Schedulers ticking now: removals wait for the sync point
    // Removals made while a call runs wait for the host's outermost eval or run to return
    // (and while a Scheduler ticks, for its sync point), and so does what a redefinition
    // replaced: a call may still be running on the object, or from the definition.
    Vec<ObjectHandle> deferred{alloc};
    Vec<Vec<detail::Overload>> retired_overloads{alloc};
    Vec<Vec<ScriptClass::Handler>> retired_handlers{alloc};
    bool unsettled = false;

    // What run_autorun's commands replaced, to restore when they run again.
    struct SavedFunction {
        Symbol name;
        bool existed = false;  // false: undefined before, so undefined after
        Tier tier = Tier::Native;
        Status status = Status::Normal;
        Symbol replacement;
        Str doc;
        Vec<detail::Overload> overloads;
        Symbol file;
        int32_t line = 0;
    };
    struct SavedClass {
        Symbol name;
        Vec<ScriptClass::Handler> handlers;
    };
    Vec<SavedFunction> autorun_functions{alloc};
    Vec<SavedClass> autorun_classes{alloc};
    bool in_autorun = false;

    // Context::call's commands, {object method args...}, one per size below kCallSizes,
    // reused while nothing else holds them: a C++ call into script allocates nothing.
    static constexpr size_t kCallSizes = 8;
    Ref<Array> call_arrays[kCallSizes];
};

// Restores every variable bound since it was made: dynamic scope, undone even on error.
class Rebind {
public:
    explicit Rebind(Context::Impl& impl) noexcept : impl_(impl), mark_(impl.saved.size()) {}
    ~Rebind() {
        while (impl_.saved.size() > mark_) {
            auto& [cell, old] = impl_.saved.back();
            *cell = std::move(old);
            impl_.saved.pop_back();
        }
    }
    Rebind(const Rebind&) = delete;
    Rebind& operator=(const Rebind&) = delete;

private:
    Context::Impl& impl_;
    size_t mark_;
};

}  // namespace baker
