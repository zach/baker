// Objects: the per-Context object table, method dispatch, properties and loose parameters,
// and the {class}, {new} and {delete} built-ins.

#include "common/containers.hpp"
#include "internal.hpp"

#include <functional>

namespace baker {

using detail::ObjectParams;

namespace {

// An instance of a script-only class.
struct ScriptObject final : Object<ScriptObject> {};

// A bound property, else a loose parameter. Null if neither exists.
std::optional<Node> get_property(ObjectBase& o, Symbol name, ObjectParams* params) {
    if (const detail::PropertyEntry* p = o.type().property(name))
        return p->get(p->accessor.get(), o);
    if (params)
        for (const auto& [k, v] : params->items)
            if (k == name) return v;
    return std::nullopt;
}

}  // namespace

// --- TypeRecord ---

namespace detail {

const MethodEntry* TypeRecord::method(Symbol method_name) const noexcept {
    auto it = std::lower_bound(
        methods.begin(), methods.end(), method_name,
        [](const MethodEntry& m, Symbol s) { return std::less<>()(m.name.id(), s.id()); });
    return it != methods.end() && it->name == method_name ? &*it : nullptr;
}

const PropertyEntry* TypeRecord::property(Symbol property_name) const noexcept {
    for (const PropertyEntry& p : properties)
        if (p.name == property_name) return &p;
    return nullptr;
}

void TypeRecord::sort() {
    std::sort(methods.begin(), methods.end(), [](const MethodEntry& a, const MethodEntry& b) {
        return std::less<>()(a.name.id(), b.name.id());
    });
    std::sort(messages.begin(), messages.end(), [](const MessageEntry& a, const MessageEntry& b) {
        return std::less<>()(a.key, b.key);
    });
    symbol = Symbol(name);
}

TypeRecord* new_type_record(std::string_view name) {
    // One per bound type, for the life of the program: from the default allocator.
    auto* r = detail::create<TypeRecord>(default_allocator());
    r->name = name;
    return r;
}

void set_type_name(TypeRecord& type, std::string_view name) {
    type.name = name;
}

MethodEntry* add_method(TypeRecord& r, Symbol name, Overload o) {
    MethodEntry* m = nullptr;
    for (MethodEntry& x : r.methods)
        if (x.name == name) {
            if (x.inherited) x = MethodEntry{name, {}, {}, {}, {}, false, {}};
            m = &x;
            break;
        }
    if (!m) {
        r.methods.push_back(MethodEntry{name, {}, {}, {}, {}, false, {}});
        m = &r.methods.back();
    }
    auto has_raw = [](const Overload& x) {
        return std::any_of(x.params.begin(), x.params.end(),
                           [](const ParamInfo& p) { return p.raw; });
    };
    if (!m->overloads.empty() &&
        (has_raw(o) || std::any_of(m->overloads.begin(), m->overloads.end(), has_raw)))
        r.error = "method '" + Str(name.str()) + "' of " + r.name +
                  " takes unevaluated arguments, so it cannot be overloaded";
    m->overloads.push_back(std::move(o));
    return m;
}

void set_method_doc(MethodEntry& m, std::string_view doc) {
    m.doc = doc;
}

void set_method_status(MethodEntry& m, Status status, Symbol replacement) {
    m.status = status;
    m.replacement = replacement;
}

void add_property(TypeRecord& r, Symbol name, std::string_view value_type, Shared accessor,
                  PropertyGet get, PropertySet set, std::string_view doc) {
    PropertyEntry p{name, value_type, std::move(accessor), get, set, Str(doc)};
    for (PropertyEntry& x : r.properties)
        if (x.name == name) {
            x = std::move(p);
            return;
        }
    r.properties.push_back(std::move(p));
}

void inherit(TypeRecord& r, const TypeRecord& b) {
    if (r.is(&b)) return;
    r.bases.push_back(&b);
    r.bases.insert(r.bases.end(), b.bases.begin(), b.bases.end());
    for (const MessageEntry& m : b.messages)
        if (std::none_of(r.messages.begin(), r.messages.end(),
                         [&](const MessageEntry& x) { return x.key == m.key; }))
            r.messages.push_back(m);
    for (MethodEntry m : b.methods) {
        if (std::any_of(r.methods.begin(), r.methods.end(),
                        [&](const MethodEntry& x) { return x.name == m.name; }))
            continue;
        m.inherited = true;
        r.methods.push_back(std::move(m));
    }
    for (const PropertyEntry& p : b.properties)
        if (std::none_of(r.properties.begin(), r.properties.end(),
                         [&](const PropertyEntry& x) { return x.name == p.name; }))
            r.properties.push_back(p);
}

void set_create(TypeRecord& r, CreateObject create, DestroyObject destroy) {
    r.create = create;
    r.destroy = destroy;
}

void add_message(TypeRecord& r, const void* key, MessageThunk thunk, Shared callable) {
    for (MessageEntry& m : r.messages)
        if (m.key == key) {
            m = MessageEntry{key, thunk, std::move(callable)};
            return;
        }
    r.messages.push_back(MessageEntry{key, thunk, std::move(callable)});
}

MessageTarget find_message(const TypeRecord& r, const void* key) noexcept {
    // Sorted by key: a type handles few messages, so this is a short scan or search.
    auto it = std::lower_bound(
        r.messages.begin(), r.messages.end(), key,
        [](const MessageEntry& m, const void* k) { return std::less<>()(m.key, k); });
    if (it == r.messages.end() || it->key != key) return {};
    return {it->thunk, it->callable.get()};
}

void set_method_message(MethodEntry& m, std::string_view message_type) {
    m.message = message_type;
}

bool has_method(const TypeRecord& r, Symbol name) noexcept {
    return r.method(name) != nullptr;
}

bool scripted(const ObjectBase& o, Symbol name) noexcept {
    Context::Impl& impl = o.ctx_->impl();
    if (!impl.handled(name)) return false;  // before the slot: the C++ path's one test
    const Context::Impl::Slot* s = impl.slot(o.handle_);
    return s && impl.handler_for(*s, name);
}

Error unhandled(const ObjectBase& o, Symbol name) {
    return make_error(ErrorCode::NotFound,
                      Str(o.type_name()) + " does not handle the message " + quoted(name));
}

void finish_type_record(TypeRecord& r) {
    r.sort();
}

bool type_is(const TypeRecord& type, const TypeRecord* other) noexcept {
    return type.is(other);
}

std::string_view type_record_name(const TypeRecord& type) noexcept {
    return type.name;
}

}  // namespace detail

ObjectBase::~ObjectBase() {
    // At once, even during a tick: the deferral that lets later tasks still reach a deleted
    // object cannot keep a destroyed one, and the sync point would write into it.
    if (ctx_) ctx_->impl().remove(handle_);  // which frees the loose parameters
}

// --- Impl ---

Expected<Node> Context::Impl::add(ObjectBase& object, Symbol name, ScriptClass* script,
                                  bool owned) {
    const detail::TypeRecord& type = object.type();
    if (!type.error.empty()) return Unexpected(make_error(ErrorCode::Refused, type.error));
    if (object.ctx_)
        return Unexpected(make_error(ErrorCode::Refused, "the object is already in a Context"));
    if (!name.empty()) {
        if (const Function* f = find(name); f && !f->overloads.empty())
            return Unexpected(make_error(
                ErrorCode::Refused, quoted(name) + " is a function, so it cannot name an object"));
        if (slot(named(name)))
            return Unexpected(
                make_error(ErrorCode::Refused, "an object is already named " + quoted(name)));
    }
    uint32_t index;
    if (!free_slots.empty()) {
        index = free_slots.back();
        free_slots.pop_back();
    } else {
        index = static_cast<uint32_t>(slots.size());
        slots.emplace_back();
    }
    Slot& s = slots[index];
    s.object = &object;
    s.name = name;
    s.script = script;
    s.owned = owned;
    object.ctx_ = &self;
    object.handle_ = ObjectHandle::make(index, s.salt);
    if (!name.empty()) {
        if (name.index() >= by_name.size()) by_name.resize(name.index() + 1);
        by_name[name.index()] = object.handle_;
    }
    if (!script && std::find(types.begin(), types.end(), &type) == types.end())
        types.push_back(&type);
    return Node::object(object.handle_);
}

// An object leaving this Context: it forgets the Context, and its loose parameters, which
// are the Context's, are freed.
void Context::Impl::forget(ObjectBase& o) noexcept {
    o.ctx_ = nullptr;
    if (o.params_) {
        detail::destroy(alloc, o.params_);
        o.params_ = nullptr;
    }
}

void Context::Impl::remove(ObjectHandle h) noexcept {
    Slot* s = slot(h);
    if (!s) return;
    if (!s->name.empty() && s->name.index() < by_name.size()) by_name[s->name.index()] = {};
    forget(*s->object);
    *s = Slot{nullptr, nullptr, {}, s->salt + 1 == 0 ? 1 : s->salt + 1, false};
    free_slots.push_back(h.index());
}

// Script handlers: the object's own class, else classes named after its type or an
// ancestor's, most derived first.
const ScriptClass::Handler* Context::Impl::handler_for(const Slot& s, Symbol method) const {
    if (s.script) return s.script->handler(method);
    if (!handled(method)) return nullptr;  // the usual answer, for one load
    const detail::TypeRecord& type = s.object->type();
    for (size_t i = 0; i <= type.bases.size(); ++i) {
        const detail::TypeRecord& t = i == 0 ? type : *type.bases[i - 1];
        if (const ScriptClass* c = script_class(t.symbol))
            if (const ScriptClass::Handler* h = c->handler(method)) return h;
    }
    return nullptr;
}

ScriptClass* Context::Impl::script_class(Symbol name) const noexcept {
    const uint32_t i = name.index();
    return i < class_by_name.size() ? class_by_name[i] : nullptr;
}

const detail::TypeRecord* Context::Impl::type_named(Symbol name) const noexcept {
    for (const detail::TypeRecord* t : types)
        if (t->symbol == name) return t;
    return nullptr;
}

Str Context::Impl::class_name(const Slot& s) const {
    return s.script ? Str(s.script->name.str()) : s.object->type().name;
}

// {object method args...}: script handlers, then native methods, then the methods every
// object has.
Expected<Node> Context::Impl::send(ObjectHandle target, const Array& command, Counters** counted) {
    Slot* s = slot(target);
    if (!s)
        return fail(command, ErrorCode::NotFound,
                    "the object this command is sent to was removed (a stale handle)");
    if (command.size() < 2 || command[1].type() != Type::Symbol)
        return fail(command, ErrorCode::Syntax,
                    "an object command names a method: {object method ...}");
    const Symbol method = *command[1].try_sym();
    ObjectBase* object = s->object;
    const detail::TypeRecord& type = object->type();
    const size_t n = command.size() - 2;

    if (const ScriptClass::Handler* handler = handler_for(*s, method)) {
        if constexpr (!kShipping) {
            ++handler->counters.calls;
            if (counted) *counted = &handler->counters;
        }
        if (const auto params = uint32_t(handler->body.params.size()); n != params)
            return fail(command, ErrorCode::Type, arity_message(method, params, params, n));
        Args args(self, command, command.begin() + 2, n, 0, false, 2, object);
        const Node this_node = Node::object(target);
        Expected<Node> r = run_script(handler->body, args, &this_node);
        if (!r && r.error().file().empty()) {
            r.error().set_location(command.file(), command.line());
        }
        return r;
    }
    if (const detail::MethodEntry* m = type.method(method))
        return invoke_set(method, m->overloads, command, 2, object);

    // The methods every object has. Evaluating an argument can move `s` ({new} grows the
    // slots), though not remove the object: removals wait for the host's eval.
    static const Symbol kGet("get"), kSet("set"), kType("type"), kName("name");
    Args args(self, command, command.begin() + 2, n, 0, false, 2, object);
    if (method == kType || method == kName) {
        if (n != 0) return fail(command, ErrorCode::Type, quoted(method) + " takes no arguments");
        if (method == kType) return Node(Symbol(class_name(*s)));
        return s->name.empty() ? Node() : Node(s->name);
    }
    if (method == kGet || method == kSet) {
        if (const uint32_t want = method == kGet ? 1 : 2; n != want)
            return fail(command, ErrorCode::Type, arity_message(method, want, want, n));
        auto key = args.get<Symbol>(0);
        if (!key) return Unexpected(std::move(key).error());
        if (method == kGet) {
            if (auto v = get_property(*object, *key, object->params_)) return *v;
            return fail(
                command, ErrorCode::NotFound,
                class_name(*slot(target)) + " has no property or parameter " + quoted(*key));
        }
        auto value = args.eval(1);
        if (!value) return value;
        auto r = set_property(*object, *key, *value, command);
        if (!r) return Unexpected(std::move(r).error());
        return value;
    }
    return fail(command, ErrorCode::NotFound, class_name(*s) + " has no method " + quoted(method));
}

Expected<void> Context::Impl::set_property(ObjectBase& o, Symbol name, const Node& value,
                                           const Array& at) {
    if (const detail::PropertyEntry* p = o.type().property(name)) {
        if (p->set(p->accessor.get(), o, value)) return {};
        return Unexpected(make_error(ErrorCode::Type,
                                     "property " + quoted(name) + " of " + o.type().name + " is " +
                                         Str(p->type) + ", not " +
                                         Str(baker::type_name(value.type())),
                                     at.file(), at.line()));
    }
    if (!o.params_) o.params_ = detail::create<ObjectParams>(alloc, alloc);
    for (auto& [k, v] : o.params_->items)
        if (k == name) {
            v = value;
            return {};
        }
    o.params_->items.emplace_back(name, value);
    return {};
}

// [a b ...]: property `a` of $this, then `b` of that (an object's property, or the value
// tagged `b` in an array), and so on.
Expected<Node> Context::Impl::property_path(const Array& path) {
    const Node* self_node = cell_if(sym_this());
    Node current = self_node ? *self_node : Node();
    if (current.type() != Type::Object)
        return fail(path, ErrorCode::NotFound, "[...] reads a property of $this, which is not set");
    for (const Node& seg : path.nodes()) {
        const auto name = seg.try_sym();
        if (!name) return fail(path, ErrorCode::Syntax, "[...] holds property names");
        if (current.type() == Type::Object) {
            Slot* s = slot(current.object_handle());
            if (!s) return fail(path, ErrorCode::NotFound, "[...] reads a removed object");
            auto v = get_property(*s->object, *name, s->object->params_);
            if (!v)
                return fail(path, ErrorCode::NotFound,
                            class_name(*s) + " has no property or parameter " + quoted(*name));
            current = std::move(*v);
        } else if (const Array* a = current.try_array(); a && a->try_find(*name)) {
            const Array& tagged = *a->try_find(*name);
            current = tagged.size() > 1 ? tagged[1] : Node();
        } else {
            return fail(path, ErrorCode::NotFound, quoted(*name) + " is not found in the value");
        }
    }
    return current;
}

// --- Context ---

Expected<Node> Context::add(ObjectBase& object, Symbol name) {
    return impl_->add(object, name, nullptr, false);
}

void Context::remove(ObjectBase& object) noexcept {
    if (object.ctx_ != this) return;
    if (impl_->ticking || impl_->depth)
        impl_->defer_removal(object.handle_);
    else
        impl_->remove_now(object.handle_);
}

// Removes the object at `h`, and destroys it if the Context owns it ({new}).
void Context::Impl::remove_now(ObjectHandle h) noexcept {
    Slot* s = slot(h);
    if (!s) return;  // removed already, or destroyed by its owner
    ObjectBase* o = s->object;
    const bool was_owned = s->owned;
    remove(h);
    if (was_owned) destroy_owned(o);
}

void Context::Impl::defer_removal(ObjectHandle h) {
    deferred.push_back(h);
    unsettled = true;
}

void Context::Impl::retire(Vec<detail::Overload>& overloads) {
    if (depth) {
        retired_overloads.push_back(std::move(overloads));
        overloads = Vec<detail::Overload>(alloc);
        unsettled = true;
    }
    overloads.clear();
}

void Context::Impl::retire(Vec<ScriptClass::Handler>& handlers) {
    if (depth) {
        retired_handlers.push_back(std::move(handlers));
        handlers = retired_handlers.back();  // a copy to change; the running ones stay put
        unsettled = true;
    }
}

void Context::Impl::note_function(Symbol name) {
    if (!in_autorun) return;
    for (const SavedFunction& s : autorun_functions)
        if (s.name == name) return;  // the first change keeps what was there before
    SavedFunction entry{
        name, false, Tier::Native, Status::Normal, {}, Str(alloc), Vec<detail::Overload>(alloc),
        {},   0};
    if (const Function* f = find(name); f && !f->overloads.empty()) {
        entry.existed = true;
        entry.tier = f->tier;
        entry.status = f->status;
        entry.replacement = f->replacement;
        entry.doc = f->doc;
        entry.overloads = f->overloads;
        entry.file = f->file;
        entry.line = f->line;
    }
    autorun_functions.push_back(std::move(entry));
}

void Context::Impl::note_class(Symbol name) {
    if (!in_autorun) return;
    for (const SavedClass& s : autorun_classes)
        if (s.name == name) return;
    SavedClass entry{name, Vec<ScriptClass::Handler>(alloc)};
    if (const ScriptClass* c = script_class(name)) entry.handlers = c->handlers;
    autorun_classes.push_back(std::move(entry));
}

void Context::Impl::retract_autorun() {
    for (size_t i = autorun_functions.size(); i-- > 0;) {
        SavedFunction& s = autorun_functions[i];
        Function& f = function(s.name);
        retire(f.overloads);  // a body of it may be running
        if (s.existed) {
            f.tier = s.tier;
            f.status = s.status;
            f.replacement = s.replacement;
            f.doc = std::move(s.doc);
            f.overloads = std::move(s.overloads);
            f.file = s.file;
            f.line = s.line;
        }
        f.refresh();
    }
    for (size_t i = autorun_classes.size(); i-- > 0;)
        if (ScriptClass* c = script_class(autorun_classes[i].name)) {
            retire(c->handlers);
            c->handlers = std::move(autorun_classes[i].handlers);
        }
    autorun_functions.clear();
    autorun_classes.clear();
}

void Context::Impl::settle() {
    retired_overloads.clear();
    retired_handlers.clear();
    unsettled = false;
    if (!ticking) self.sync();  // while a Scheduler ticks, removals wait for its sync point
}

void Context::sync() {
    Impl& impl = *impl_;
    for (size_t i = 0; i < impl.deferred.size(); ++i)  // removals may defer more
        impl.remove_now(impl.deferred[i]);
    impl.deferred.clear();
}

ObjectBase* Context::try_resolve(const Node& object) const noexcept {
    if (object.type() != Type::Object) return nullptr;
    Impl::Slot* s = impl_->slot(object.object_handle());
    return s ? s->object : nullptr;
}

Node Context::object(Symbol name) const noexcept {
    const ObjectHandle h = impl_->named(name);
    return impl_->slot(h) ? Node::object(h) : Node();
}

void Context::add_type(const detail::TypeRecord& type) {
    if (std::find(impl_->types.begin(), impl_->types.end(), &type) == impl_->types.end())
        impl_->types.push_back(&type);
}

Expected<Node> Context::call(const Node& object, Symbol method, std::span<const Node> args) {
    const size_t size = args.size() + 2;
    Ref<Array>* cached = size < Impl::kCallSizes ? &impl_->call_arrays[size] : nullptr;
    // The cached command is free when the cache's is its only count (a nested call, or a
    // {func} that kept its command, holds another).
    Ref<Array> command = cached && *cached && (*cached)->ref_count() == 1
                             ? *cached
                             : Array::make(static_cast<uint32_t>(size));
    command->set(0, object);
    command->set(1, Node(method));
    for (size_t i = 0; i < args.size(); ++i) command->set(i + 2, args[i]);
    Expected<Node> r = eval(Node(command, Type::Command));
    if (cached) {
        if (command->ref_count() == (*cached == command ? 2u : 1u)) {
            // Nothing kept it: empty it, so it holds nothing alive, and keep it for next time.
            for (size_t i = 0; i < size; ++i) command->set(i, Node());
            *cached = std::move(command);
        } else if (*cached == command) {
            *cached = nullptr;  // kept by the script: let it go
        }
    }
    return r;
}

Error Context::resolve_error(const Node& object, std::string_view type) const {
    if (object.type() != Type::Object)
        return make_error(ErrorCode::Type, "expected a " + Str(type) + " object, not " +
                                               Str(baker::type_name(object.type())));
    if (!try_resolve(object))
        return make_error(ErrorCode::NotFound, "the " + Str(type) + " object was removed");
    return make_error(ErrorCode::Type, "the object is a " + try_resolve(object)->type().name +
                                           ", not a " + Str(type));
}

// --- Raw<Var> ---

Expected<void> Raw<Var>::store(Node v) const {
    if (cell_) {
        *cell_ = std::move(v);
        return {};
    }
    Context::Impl& impl = ctx_->impl();
    const Node* self_node = impl.cell_if(sym_this());
    ObjectBase* o = self_node ? ctx_->try_resolve(*self_node) : nullptr;
    if (!o)
        return Unexpected(make_error(ErrorCode::NotFound,
                                     "{set [...] value} sets a property of $this, which is not set",
                                     property_->file(), property_->line()));
    const auto name = property_->size() == 1 ? (*property_)[0].try_sym() : std::nullopt;
    if (!name)
        return Unexpected(make_error(ErrorCode::Syntax, "{set [...] value} sets one property",
                                     property_->file(), property_->line()));
    return impl.set_property(*o, *name, v, *property_);
}

Node Raw<Var>::load() const {
    if (cell_) return VarRef(cell_, name_).value();
    auto r = ctx_->impl().property_path(*property_);
    return r ? std::move(*r) : Node();
}

// --- Built-ins ---

namespace {

// The handlers of {class Name handler...} or {extend Name handler...}, from argument 1 on.
Expected<void> read_handlers(Args& a, Context::Impl& impl, Vec<ScriptClass::Handler>& out) {
    for (size_t i = 1; i < a.size(); ++i) {
        const Array* h = a.raw(i).try_array();
        if (!h || h->empty() || (*h)[0].type() != Type::Symbol)
            return Unexpected(
                a.error("a handler is (method ($params...) body...)", ErrorCode::Syntax));
        ScriptClass::Handler handler{*(*h)[0].try_sym(), ScriptBody(impl.alloc), {}};
        handler.body.definition = Ref<Array>(const_cast<Array*>(h));
        handler.body.body = 1;
        if (const Array* params = h->size() > 1 ? (*h)[1].try_array() : nullptr) {
            if (!parse_params(*params, handler.body))
                return Unexpected(
                    a.error("parameters of " + quoted(handler.name) + " must be $variables",
                            ErrorCode::Syntax));
            handler.body.body = 2;
        }
        const uint32_t bit = handler.name.index();
        if (bit / 64 >= impl.handled_names.size()) impl.handled_names.resize(bit / 64 + 1);
        impl.handled_names[bit / 64] |= uint64_t{1} << (bit % 64);
        out.push_back(std::move(handler));
    }
    return {};
}

}  // namespace

void register_object_builtins(Context& ctx) {
    // {class Name (method ($params...) body...) (method body...) ...}
    auto define_class = [](Args a) -> Expected<Node> {
        if (a.size() < 1 || a.raw(0).type() != Type::Symbol)
            return Unexpected(
                a.error("write {class Name (method ($params...) body...) ...}", ErrorCode::Syntax));
        const Symbol name = *a.raw(0).try_sym();
        Context::Impl& impl = a.context().impl();
        auto cls = Own<ScriptClass>::make(impl.alloc, impl.alloc);
        cls->name = name;
        if (auto r = read_handlers(a, impl, cls->handlers); !r) return Unexpected(r.error());
        impl.note_class(name);
        if (name.index() >= impl.class_by_name.size()) impl.class_by_name.resize(name.index() + 1);
        if (ScriptClass* old = impl.class_by_name[name.index()]) {
            impl.retire(old->handlers);
            *old = std::move(*cls);  // a redefinition replaces the handlers (a reloaded script)
        } else {
            impl.class_by_name[name.index()] = cls.get();
            impl.classes.push_back(std::move(cls));
        }
        return Node(name);
    };
    // {extend Name (method ($params...) body...) ...}: adds handlers to a class, or replaces
    // the ones of the same name, from another file. A bound type with no class yet
    // gets one.
    auto extend_class = [](Args a) -> Expected<Node> {
        if (a.size() < 1 || a.raw(0).type() != Type::Symbol)
            return Unexpected(a.error("write {extend Name (method ($params...) body...) ...}",
                                      ErrorCode::Syntax));
        const Symbol name = *a.raw(0).try_sym();
        Context::Impl& impl = a.context().impl();
        ScriptClass* cls = impl.script_class(name);
        if (!cls) {
            const detail::TypeRecord* native = impl.type_named(name);
            if (!native)
                return Unexpected(a.error(
                    quoted(name) + " is not a class: {class " + Str(name.str()) + " ...} it first",
                    ErrorCode::NotFound));
            auto made = Own<ScriptClass>::make(impl.alloc, impl.alloc);
            made->name = name;
            if (name.index() >= impl.class_by_name.size())
                impl.class_by_name.resize(name.index() + 1);
            impl.class_by_name[name.index()] = made.get();
            cls = made.get();
            impl.classes.push_back(std::move(made));
        }
        Vec<ScriptClass::Handler> added(impl.alloc);
        if (auto r = read_handlers(a, impl, added); !r) return Unexpected(r.error());
        impl.note_class(name);
        impl.retire(cls->handlers);
        for (ScriptClass::Handler& h : added) {
            ScriptClass::Handler* same = nullptr;
            for (ScriptClass::Handler& old : cls->handlers)
                if (old.name == h.name) same = &old;
            if (same)
                *same = std::move(h);
            else
                cls->handlers.push_back(std::move(h));
        }
        return Node(name);
    };
    if (auto r = ctx.func("class", define_class))
        r->doc(
            "`{class Name (method ($params...) body...) ...}`: script handlers. If `Name` is a "
            "bound C++ type, they apply to its objects and are tried before its C++ methods; "
            "otherwise `Name` is a script class for `new`. In a handler, `$this` is the "
            "object and `[name]` its property or parameter.");
    if (auto r = ctx.func("extend", extend_class))
        r->doc(
            "`{extend Name (method ($params...) body...) ...}`: adds script handlers to a class, "
            "replacing only those of the same name, so another file (a task pack) can add one. "
            "`Name` is a `{class}` or a bound C++ type. Run it after the `{class}`: a "
            "redefinition replaces every handler.");

    // {new Class [name] (param value)...}
    auto make = [](Args a) -> Expected<Node> {
        if (a.size() < 1 || a.raw(0).type() != Type::Symbol)
            return Unexpected(
                a.error("write {new Class [name] (param value)...}", ErrorCode::Syntax));
        const Symbol cls = *a.raw(0).try_sym();
        Context::Impl& impl = a.context().impl();
        ObjectBase* object = nullptr;
        ScriptClass* script = nullptr;
        if (const detail::TypeRecord* t = impl.type_named(cls)) {
            if (!t->create)
                return Unexpected(a.error(quoted(cls) + " cannot be created from script: it is not "
                                                        "default-constructible",
                                          ErrorCode::Refused));
            object = t->create(impl.alloc);
        } else if (ScriptClass* found = impl.script_class(cls)) {
            script = found;
            object = detail::create<ScriptObject>(impl.alloc);
        } else {
            return Unexpected(a.error(quoted(cls) + " is not a class", ErrorCode::NotFound));
        }
        size_t i = 1;
        Symbol name;
        if (a.size() > 1 && a.raw(1).type() == Type::Symbol) name = *a.raw(i++).try_sym();
        auto node = impl.add(*object, name, script, true);
        if (!node) {
            impl.destroy_owned(object);
            return Unexpected(a.error(Str(node.error().message()), ErrorCode::Refused));
        }
        for (; i < a.size(); ++i) {
            const Array* p = a.raw(i).try_array();
            Expected<void> r;
            if (!p || p->size() != 2 || (*p)[0].type() != Type::Symbol) {
                r = Unexpected(a.error("parameters are (name value)", ErrorCode::Syntax));
            } else if (auto v = impl.eval((*p)[1]); !v) {
                r = Unexpected(std::move(v).error());
            } else {
                r = impl.set_property(*object, *(*p)[0].try_sym(), *v, a.command());
            }
            if (!r) {
                impl.remove(node->object_handle());
                impl.destroy_owned(object);
                return Unexpected(std::move(r).error());
            }
        }
        return node;
    };
    if (auto r = ctx.func("new", make))
        r->doc(
            "`{new Class [name] (param value)...}`: creates an object of a script class or of "
            "a default-constructible bound type, owned by the Context, and returns it.");

    auto destroy = [](Context& c, Node object) -> Expected<Node> {
        Context::Impl& impl = c.impl();
        if (auto name = object.try_sym()) object = c.object(*name);  // {delete g1}
        if (object.type() != Type::Object)
            return Unexpected(make_error(
                ErrorCode::Type, "delete takes an object, not a " + Str(type_name(object.type()))));
        Context::Impl::Slot* s = impl.slot(object.object_handle());
        if (!s) return Node();
        // When the host's eval returns, or during a tick at its end: a call may
        // still be running on the object, and later tasks in the tick still reach it.
        impl.defer_removal(object.object_handle());
        return Node();
    };
    if (auto r = ctx.func("delete", destroy))
        r->doc(
            "`{delete object}`: removes the object (or the object of that name) when the "
            "host's eval returns, or at the end of a tick; one `new` created is destroyed. Its "
            "handles go stale.");
}

}  // namespace baker
