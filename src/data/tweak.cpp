#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/tweak.hpp>
#include <baker/write.hpp>

#include "common/containers.hpp"

#include "text.hpp"

#include <algorithm>
#include <optional>
#include <thread>

namespace baker {
namespace {

constinit TweakRegistry g_registry;

// The metadata keys, interned once: a panel reads them for every tweak every frame.
struct Keys {
    Symbol range{"range"}, step{"step"}, desc{"desc"}, note{"note"}, kind{"kind"}, group{"group"},
        one_of{"one_of"};
};
const Keys& keys() {
    static const Keys k;
    return k;
}

// A tweak's value without the metadata written after it: (color 1 0.5 0 (range 0 1)) is
// three numbers. Only trailing arrays tagged with the keys a tweak reads are left out,
// so a value whose own elements are tagged arrays reads as before.
Values without_metadata(const Values& v) {
    const Keys& k = keys();
    size_t n = v.size();
    for (; n > 0; --n) {
        const Array* a = v[n - 1].try_array();
        if (!a || a->empty()) break;
        const Node& tag = (*a)[0];
        if (!(tag.is(k.range) || tag.is(k.step) || tag.is(k.desc) || tag.is(k.note) ||
              tag.is(k.kind) || tag.is(k.group) || tag.is(k.one_of)))
            break;
    }
    return Values(v.nodes().first(n), v.source());
}

}  // namespace

// --- TweakRegistry ---

TweakRegistry& TweakRegistry::global() noexcept {
    return g_registry;
}

void TweakRegistry::lock() const noexcept {
    while (lock_.test_and_set(std::memory_order_acquire)) {
    }
}

void TweakRegistry::unlock() const noexcept {
    lock_.clear(std::memory_order_release);
}

size_t TweakRegistry::size() const noexcept {
    size_t n = 0;
    lock();
    for (const TweakBase* t = head_; t; t = t->next_) ++n;
    unlock();
    return n;
}

// --- TweakBase ---

struct TweakBase::Extra {
    std::optional<Path> parsed;
    Vec<Callback<void()>> on_change;
    bool notifying = false;  // a change made from inside a callback does not notify again
};

void TweakBase::attach() {
    registry_->lock();
    next_ = registry_->head_;
    if (next_) next_->prev_ = this;
    registry_->head_ = this;
    registry_->unlock();
    if (Config* c = registry_->config()) c->bind(*this);
}

void TweakBase::detach() noexcept {
    registry_->lock();
    // A tweak never attached (a described one whose default did not read) is in no list;
    // unlinking it would empty the registry's.
    if (prev_ || registry_->head_ == this) {
        if (prev_)
            prev_->next_ = next_;
        else
            registry_->head_ = next_;
        if (next_) next_->prev_ = prev_;
    }
    prev_ = next_ = nullptr;
    registry_->unlock();
    detail::destroy(default_allocator(), extra_);
    extra_ = nullptr;
}

const Path& TweakBase::parsed() {
    if (!extra_) extra_ = detail::create<Extra>(default_allocator());
    if (!extra_->parsed) extra_->parsed.emplace(path_);
    return *extra_->parsed;
}

bool TweakBase::unbind() {
    if (!bound_) return false;
    bound_.reset();
    reset();
    return true;
}

bool TweakBase::bind(const Array& root, const Callback<void(const Error&)>& warn) {
    const Path& path = parsed();
    if constexpr (kShipping) {
        if (meta_.dev_only) return unbind();
    }
    detail::Resolved r;
    const auto status = detail::resolve(root, path, r);
    if (status != detail::ResolveStatus::Ok) {
        if (status == detail::ResolveStatus::Malformed && warn)
            warn(detail::get_error(root, path, nullptr));
        return unbind();
    }
    const Values values = without_metadata(r.values);
    const Array* holder = values.source();
    const auto first = static_cast<uint32_t>(values.begin() - holder->begin());
    const auto count = static_cast<uint32_t>(values.size());
    bool same = bound_ && count == count_;
    for (uint32_t i = 0; same && i < count; ++i)
        same = structurally_equal((*bound_)[first_ + i], values[i]);
    if (!same && !assign(values, true)) {
        if (warn) {
            const detail::ConvertInfo want = info();
            warn(detail::get_error(root, path, &want));
        }
        return unbind();
    }
    bound_ = Ref<Array>(const_cast<Array*>(holder));
    first_ = first;
    count_ = count;
    return !same;
}

void TweakBase::notify() {
    if (!extra_ || extra_->notifying) return;
    extra_->notifying = true;
    // By index, over the count at entry: a callback may add one (it runs next time), and
    // the table may move when it does.
    const size_t n = extra_->on_change.size();
    for (size_t i = 0; i < n; ++i) extra_->on_change[i]();
    extra_->notifying = false;
}

void TweakBase::on_change(Callback<void()> callback) {
    if (!extra_) extra_ = detail::create<Extra>(default_allocator());
    extra_->on_change.push_back(std::move(callback));
}

Expected<void> TweakBase::set_values(std::span<const Node> values) {
    if constexpr (kShipping)
        return Unexpected(
            make_error(ErrorCode::Refused, "live edits are disabled under BAKER_SHIPPING"));
    const Values v(values, nullptr);
    if (!assign(v, false)) {
        Str text;
        for (const Node& n : values) text += (text.empty() ? "" : " ") + detail::preview(n);
        return Unexpected(make_error(ErrorCode::Type, "'" + Str(path_) + "' cannot be set to (" +
                                                          text + "), which is not " +
                                                          Str(info().name)));
    }
    if (Config* c = registry_->config()) return c->set(parsed(), values);
    registry_->begin_change();
    (void)assign(v, true);
    registry_->end_change();
    notify();
    return {};
}

Expected<Source> TweakBase::source() const {
    Config* c = registry_->config();
    if (!c)
        return Unexpected(make_error(ErrorCode::NotFound, "'" + Str(path_) + "' has no config"));
    return c->source(const_cast<TweakBase*>(this)->parsed());
}

Expected<void> TweakBase::write_back() {
    Config* c = registry_->config();
    if (!c)
        return Unexpected(make_error(ErrorCode::NotFound, "'" + Str(path_) + "' has no config"));
    if (auto r = c->write_back(parsed()); !r) return Unexpected(std::move(r).error());
    return {};
}

// --- Metadata: the data file's, else the code's. ---

const Array* TweakBase::meta_array(Symbol key) const {
    return bound_ ? bound_->try_find(key) : nullptr;
}

TweakRange TweakBase::range() const {
    if (const Array* a = meta_array(keys().range); a && a->size() >= 3) {
        const auto lo = (*a)[1].try_float(), hi = (*a)[2].try_float();
        if (lo && hi) return {*lo, *hi};
    }
    return meta_.range;
}

double TweakBase::step() const {
    if (const Array* a = meta_array(keys().step); a && a->size() >= 2)
        if (auto v = (*a)[1].try_float()) return *v;
    return meta_.step;
}

namespace {
// A string or symbol value of `a`, or empty.
std::string_view text_value(const Array* a) {
    if (!a || a->size() < 2) return {};
    if (auto s = (*a)[1].try_str()) return *s;
    if (auto s = (*a)[1].try_sym()) return s->str();
    return {};
}
}  // namespace

std::string_view TweakBase::desc() const {
    const auto d = text_value(meta_array(keys().desc));
    return d.empty() ? meta_.desc : d;
}

std::string_view TweakBase::note() const {
    return text_value(meta_array(keys().note));
}

namespace {
// The presentation rule, given the value, its kind and its range: a kind given wins; a
// 3- or 4-number value ranged 0..1 is a color (after SeExpr's editor).
std::string_view kind_of(std::span<const Node> values, std::string_view kind, TweakRange r) {
    if (!kind.empty()) return kind;
    if (values.size() != 3 && values.size() != 4) return {};
    for (const Node& n : values)
        if (!n.is_number()) return {};
    return r.min == 0 && r.max == 1 ? "color" : std::string_view();
}
}  // namespace

std::string_view TweakBase::kind() const {
    std::string_view k = text_value(meta_array(keys().kind));
    if (k.empty()) k = meta_.kind;
    const std::span<const Node> values =
        bound_ ? std::span<const Node>(bound_->nodes().subspan(first_, count_))
               : std::span<const Node>();
    return kind_of(values, k, range());
}

ValueMetadata split_metadata(const Values& all) noexcept {
    const Values values = without_metadata(all);
    return {values, all.nodes().subspan(values.size())};
}

std::string_view presented_kind(const Values& all, std::string_view kind,
                                TweakRange range) noexcept {
    const ValueMetadata vm = split_metadata(all);
    for (const Node& n : vm.metadata) {
        const Array& a = *n.try_array();
        if (a[0].is(keys().kind) && a.size() >= 2) {
            if (const auto s = a[1].try_sym()) kind = s->str();
            if (const auto t = a[1].try_str()) kind = *t;
        } else if (a[0].is(keys().range) && a.size() >= 3) {
            const auto lo = a[1].try_float(), hi = a[2].try_float();
            if (lo && hi) range = {*lo, *hi};
        }
    }
    return kind_of(vm.values.nodes(), kind, range);
}

namespace {
// What a tweak may be, from (one_of ...) after its value if the data has it, else from its
// code: symbols or strings, as text; integers, with their names.
struct Allowed {
    Vec<Symbol> texts;
    Vec<TweakChoice> numbers;
};
Allowed allowed(const Array* data, const TweakMeta& code) {
    Allowed out;
    if (data) {
        for (const Node& n : data->values()) {
            if (const auto s = n.try_sym()) {
                out.texts.push_back(*s);
            } else if (const auto t = n.try_str()) {
                out.texts.emplace_back(*t);
            } else if (const auto i = n.try_int()) {
                out.numbers.push_back({*i, {}});
            } else if (const Array* c = n.try_array(); c && c->size() == 2 && (*c)[0].try_int()) {
                std::string_view name;
                if (const auto sym = (*c)[1].try_sym()) name = sym->str();
                if (const auto str = (*c)[1].try_str()) name = Symbol(*str).str();  // interned
                out.numbers.push_back({*(*c)[0].try_int(), name});
            }
        }
        return out;
    }
    for (const std::string_view s : code.one_of) out.texts.emplace_back(s);
    for (const TweakChoice& c : code.choices) out.numbers.push_back(c);
    return out;
}
}  // namespace

List<Symbol> TweakBase::one_of() const {
    const Allowed a = allowed(meta_array(keys().one_of), meta_);
    return List<Symbol>({a.texts.data(), a.texts.size()});
}

List<TweakChoice> TweakBase::choices() const {
    Allowed a = allowed(meta_array(keys().one_of), meta_);
    for (TweakChoice& c : a.numbers) c.name = Symbol(c.name).str();  // the code's, interned
    return List<TweakChoice>({a.numbers.data(), a.numbers.size()});
}

std::string_view TweakBase::group() const {
    if (const auto g = text_value(meta_array(keys().group)); !g.empty()) return g;
    if (!meta_.group.empty()) return meta_.group;
    const std::string_view p = path_;
    const size_t cut = p.find_last_of("./");
    return cut == std::string_view::npos ? std::string_view() : p.substr(0, cut);
}

// --- Holding data to the tweaks ---

namespace {

// The metadata array tagged `key` written after a value, as (range 0 10) in
// (speed 4 (range 0 10)); null if none.
const Array* trailing_meta(const Values& all, Symbol key) {
    for (size_t n = all.size(); n > 0; --n) {
        const Array* a = all[n - 1].try_array();
        if (!a || a->empty()) break;
        if ((*a)[0].is(key)) return a;
    }
    return nullptr;
}

// The path without its last segment, or empty for a top-level tweak; separators '.' and '/'.
std::string_view parent_of(std::string_view path) {
    const size_t cut = path.find_last_of("./");
    return cut == std::string_view::npos ? std::string_view() : path.substr(0, cut);
}

// The segment of `path` right after `group` and its separator, or empty if `path` is not
// under `group`.
std::string_view child_under(std::string_view path, std::string_view group) {
    if (path.size() <= group.size() + 1 || path.substr(0, group.size()) != group ||
        (path[group.size()] != '.' && path[group.size()] != '/'))
        return {};
    const std::string_view rest = path.substr(group.size() + 1);
    return rest.substr(0, rest.find_first_of("./"));
}

Error located(ErrorCode code, const Str& message, const Array* where) {
    return where ? make_error(code, message, where->file(), where->line())
                 : make_error(code, message);
}

}  // namespace

TweakFindings check_tweaks(const Array& root, const TweakRegistry& tweaks) {
    const Keys& k = keys();
    Vec<Error> errors, warnings;
    Vec<std::string_view> paths;
    tweaks.each([&](TweakBase& t) {
        // Each finding names its tweak as its path, so a tool groups and locates it.
        auto add = [&](Vec<Error>& list, Error e) {
            e.set_path(t.path());
            list.push_back(std::move(e));
        };
        paths.push_back(t.path());
        const std::string_view path = t.path();
        detail::Resolved r;
        const auto status = detail::resolve(root, t.parsed(), r);
        if (status == detail::ResolveStatus::Missing) {
            add(warnings, make_error(ErrorCode::NotFound, "the data has no '" + Str(path) +
                                                              "': the tweak keeps its default"));
            return;
        }
        if (status == detail::ResolveStatus::Malformed) {
            add(errors, detail::get_error(root, t.parsed(), nullptr));
            return;
        }
        const Values values = without_metadata(r.values);
        const Array* where = values.source();
        if (!t.assign(values, false)) {
            const detail::ConvertInfo want = t.info();
            add(errors, detail::get_error(root, t.parsed(), &want));
            return;
        }
        // The range and the list: the data's metadata, else the code's.
        TweakRange range = t.meta_.range;
        if (const Array* a = trailing_meta(r.values, k.range); a && a->size() >= 3) {
            const auto lo = (*a)[1].try_float(), hi = (*a)[2].try_float();
            if (lo && hi) range = {*lo, *hi};
        }
        const Allowed may = allowed(trailing_meta(r.values, k.one_of), t.meta_);
        const Vec<Symbol>& one_of = may.texts;
        // Each value; a list of the game's own type's elements, each of their values.
        const ListElement element = t.list_element();
        const bool nested = !element.type.empty() && !detail::value_type_known(element.type);
        auto judge = [&](const Node& v) {
            const auto x = v.try_float();
            if (!range.empty() && x && (*x < range.min || *x > range.max))
                add(errors,
                    located(ErrorCode::Range,
                            "'" + Str(path) + "' is " + detail::text_of(v) +
                                ", outside its tweak's range " + detail::text_of(Node(range.min)) +
                                " to " + detail::text_of(Node(range.max)),
                            where));
            // A symbol or a string against the texts; an integer against the choices.
            std::optional<std::string_view> text;
            if (const auto sym = v.try_sym()) text = sym->str();
            if (const auto str = v.try_str()) text = *str;
            if (!one_of.empty() && text &&
                std::none_of(one_of.begin(), one_of.end(),
                             [&](Symbol n) { return n.str() == *text; })) {
                Str names;
                for (const Symbol n : one_of) names += (names.empty() ? "" : ", ") + Str(n.str());
                add(errors, located(ErrorCode::Range,
                                    "'" + Str(path) + "' is '" + Str(*text) +
                                        "', not one of its tweak's " + names,
                                    where));
            }
            const auto number = v.try_int();
            if (!may.numbers.empty() && number && v.type() == Type::Int &&
                std::none_of(may.numbers.begin(), may.numbers.end(),
                             [&](const TweakChoice& c) { return c.value == *number; })) {
                Str names;
                for (const TweakChoice& c : may.numbers) {
                    names += (names.empty() ? "" : ", ") + detail::decimal(c.value);
                    if (!c.name.empty()) names += " (" + Str(c.name) + ")";
                }
                add(errors, located(ErrorCode::Range,
                                    "'" + Str(path) + "' is " + detail::text_of(v) +
                                        ", not one of its tweak's " + names,
                                    where));
            }
        };
        // A curve's range holds each point's y; its x and its mode are not ranged.
        const bool curve = t.info().name == "curve";
        for (const Node& v : values) {
            if (curve) {
                if (const Array* p = v.try_array(); p && p->size() >= 2) judge((*p)[1]);
                continue;
            }
            const Array* a = nested ? v.try_array() : nullptr;
            if (!a) {
                judge(v);
                continue;
            }
            for (const Node& inner : a->nodes()) judge(inner);
        }
    });
    // Keys beside a group's tweaks that no tweak reads: a misspelled key looks so.
    Vec<std::string_view> groups;
    for (const std::string_view p : paths) {
        const std::string_view g = parent_of(p);
        if (g.empty() || g.find('\'') != std::string_view::npos) continue;  // top level, or quoted
        if (std::find(groups.begin(), groups.end(), g) == groups.end()) groups.push_back(g);
    }
    for (const std::string_view g : groups) {
        detail::Resolved r;
        if (detail::resolve(root, Path(g), r) != detail::ResolveStatus::Ok || !r.array) continue;
        Vec<std::string_view> known;
        for (const std::string_view p : paths)
            if (const std::string_view c = child_under(p, g);
                !c.empty() && std::find(known.begin(), known.end(), c) == known.end())
                known.push_back(c);
        std::sort(known.begin(), known.end());  // independent of registration order
        for (const Node& n : r.array->values()) {
            const Array* child = n.try_array();
            if (!child || child->tag().empty()) continue;
            const std::string_view tag = child->tag().str();
            if (std::find(known.begin(), known.end(), tag) != known.end()) continue;
            Str read;
            for (const std::string_view c : known) read += (read.empty() ? "" : ", ") + Str(c);
            // Its path is the key's own: the group, then the key.
            Error stray = located(ErrorCode::NotFound,
                                  "'" + Str(tag) + "' in '" + Str(g) +
                                      "' is read by no tweak; its tweaks read " + read,
                                  child);
            stray.set_path(Str(g) + "." + Str(tag));
            warnings.push_back(std::move(stray));
        }
    }
    // In check --json's order, by file, line, column, then message: the same report whatever
    // order the tweaks were registered in.
    auto by_place = [](const Error& a, const Error& b) {
        if (a.file() != b.file()) return a.file().str() < b.file().str();
        if (a.line() != b.line()) return a.line() < b.line();
        if (a.column() != b.column()) return a.column() < b.column();
        return a.message() < b.message();
    };
    std::sort(errors.data(), errors.data() + errors.size(), by_place);
    std::sort(warnings.data(), warnings.data() + warnings.size(), by_place);
    return {List<Error>({errors.data(), errors.size()}),
            List<Error>({warnings.data(), warnings.size()})};
}

// --- As JSON (baker.tweaks/1.5) ---

namespace {
// The tweak's values as one text: a single value bare, several in parentheses.
// One value bare; several, or a list's elements however many, in parentheses: a list of
// one vec2 is ((0 0)), not (0 0), which reads as two numbers.
Str values_text(const ValueList& v, bool list) {
    const auto nodes = v.nodes();
    if (nodes.size() == 1 && !list) return detail::text_of(nodes[0]);
    return detail::text_of(Node(Array::make_copy(nodes)));
}

void json_number(Str& out, double d) {
    out += detail::text_of(Node(d));
}
}  // namespace

void write_json(const TweakRegistry& tweaks, TextSink text) {
    Vec<const TweakBase*> ts;
    tweaks.each([&](const TweakBase& t) { ts.push_back(&t); });
    std::sort(ts.begin(), ts.end(),
              [](const TweakBase* a, const TweakBase* b) { return a->path() < b->path(); });
    Str out = "{\n  \"schema\": \"baker.tweaks/1.5\",\n  \"tweaks\": [";
    for (size_t i = 0; i < ts.size(); ++i) {
        const TweakBase& t = *ts[i];
        ValueList def, cur;
        t.default_values(def);
        t.values(cur);
        out += i ? ",\n    {\"path\": " : "\n    {\"path\": ";
        detail::json_string(out, t.path());
        out += ", \"type\": ";
        detail::json_string(out, t.type_name());
        out += ", \"values\": ";
        if (const auto types = t.value_types(); types.empty()) {
            out += "null, \"count\": null";
        } else {
            out += '[';
            for (size_t k = 0; k < types.size(); ++k) {
                if (k) out += ", ";
                detail::json_string(out, types[k]);
            }
            out += "], \"count\": ";
            detail::json_string(out, detail::count_name(t.value_count()));
        }
        detail::json_list_element(out, t.list_element());
        out += ", \"default\": ";
        const bool list = !t.list_element().type.empty();
        detail::json_string(out, values_text(def, list));
        out += ", \"value\": ";
        detail::json_string(out, values_text(cur, list));
        out += Str(", \"bound\": ") + (t.bound() ? "true" : "false") + ", \"range\": ";
        if (const TweakRange r = t.range(); !r.empty()) {
            out += '[';
            json_number(out, r.min);
            out += ", ";
            json_number(out, r.max);
            out += ']';
        } else {
            out += "null";
        }
        out += ", \"step\": ";
        json_number(out, t.step());
        out += ", \"desc\": ";
        detail::json_string(out, t.desc());
        out += ", \"kind\": ";
        detail::json_string(out, t.kind());
        out += ", \"group\": ";
        detail::json_string(out, t.group());
        out += Str(", \"dev_only\": ") + (t.code_meta().dev_only ? "true" : "false");
        out += ", \"one_of\": ";
        const List<Symbol> one_of = t.one_of();
        detail::json_names(out, one_of.items());
        out += ", \"choices\": [";
        const List<TweakChoice> choices = t.choices();
        for (size_t k = 0; k < choices.size(); ++k) {
            out += k ? ", {\"value\": " : "{\"value\": ";
            out += detail::decimal(choices[k].value) + ", \"name\": ";
            detail::json_string(out, choices[k].name);
            out += '}';
        }
        out += ']';
        out += ", \"source\": ";
        std::optional<Source> source;
        if (t.bound())
            if (auto s = t.source()) source = *s;
        if (source) {
            out += "{\"layer\": ";
            detail::json_string(out, source->layer.str());
            out += ", \"file\": ";
            detail::json_string(out, source->file.str());
            out += ", \"line\": " + detail::decimal(source->line) + "}";
        } else {
            out += "null";
        }
        out += '}';
    }
    out += ts.empty() ? "]\n}\n" : "\n  ]\n}\n";
    text(out);
}

// --- Value types, as the reference names them (described tweaks and functions) ---

namespace {

// Whether value `i` reads as Baker's type `type`, as a game's Convert reads it with
// Values::read; nullopt if `type` is not one of Baker's single-value types.
std::optional<bool> reads_one(std::string_view type, const Values& v, size_t i) {
    auto as = [&]<class T>(std::type_identity<T>) {
        T out{};
        return v.read(i, out);
    };
    if (type == "int8") return as(std::type_identity<int8_t>());
    if (type == "uint8") return as(std::type_identity<uint8_t>());
    if (type == "int16") return as(std::type_identity<int16_t>());
    if (type == "uint16") return as(std::type_identity<uint16_t>());
    if (type == "int32") return as(std::type_identity<int32_t>());
    if (type == "uint32") return as(std::type_identity<uint32_t>());
    if (type == "int") return as(std::type_identity<int64_t>());
    if (type == "uint64") return as(std::type_identity<uint64_t>());
    if (type == "float32") return as(std::type_identity<float>());
    if (type == "float") return as(std::type_identity<double>());
    if (type == "bool") return as(std::type_identity<bool>());
    if (type == "symbol") return as(std::type_identity<Symbol>());
    if (type == "string") return as(std::type_identity<std::string_view>());
    if (type == "value") return as(std::type_identity<Node>());
    return std::nullopt;
}

}  // namespace

namespace detail {

void wait_a_moment(unsigned tries) noexcept {
    if (tries < 64) return;  // a change takes microseconds: spin a while first
    std::this_thread::yield();
}

}  // namespace detail

namespace detail {

bool value_type_known(std::string_view type) noexcept {
    const Node probe;
    return reads_one(type, Values(std::span<const Node>(&probe, 1), nullptr), 0).has_value();
}

bool reads_as(std::span<const std::string_view> types, ValueCount count, const Values& values) {
    const size_t n = types.size();
    if (count == ValueCount::Any) {
        if (n == 0 || values.size() % n != 0) return n == 0;
        for (size_t i = 0; i < values.size(); ++i)
            if (!reads_one(types[i % n], values, i).value_or(false)) return false;
        return true;
    }
    if (values.size() < n || (count == ValueCount::Exact && values.size() != n)) return false;
    for (size_t i = 0; i < n; ++i)
        if (!reads_one(types[i], values, i).value_or(false)) return false;
    return true;
}

void json_list_element(Str& out, const ListElement& element) {
    if (element.type.empty()) {
        out += R"(, "element": null, "length": null)";
        return;
    }
    out += R"(, "element": {"type": )";
    json_string(out, element.type);
    out += ", \"values\": ";
    if (element.values.empty()) {
        out += "null, \"count\": null}";
    } else {
        out += '[';
        for (size_t k = 0; k < element.values.size(); ++k) {
            if (k) out += ", ";
            json_string(out, element.values[k]);
        }
        out += "], \"count\": ";
        json_string(out, count_name(element.count));
        out += '}';
    }
    out += ", \"length\": ";
    out += element.length ? decimal(static_cast<int64_t>(element.length)) : Str("null");
}

bool reads_list(const ListElement& element, const Values& values) {
    if (element.values.empty()) return true;
    if (element.length != 0 && values.size() != element.length) return false;
    const bool nested = !value_type_known(element.type);  // a type of the game's own
    for (const Node& n : values) {
        const Array* a = nested ? n.try_array() : nullptr;
        const Values one =
            a ? Values(a->nodes(), a) : Values(std::span<const Node>(&n, 1), nullptr);
        if (!reads_as(element.values, element.count, one)) return false;
    }
    return true;
}

std::string_view count_name(ValueCount count) noexcept {
    switch (count) {
        case ValueCount::Exact: return "exact";
        case ValueCount::AtLeast: return "at_least";
        case ValueCount::Any: return "any";
    }
    return "exact";
}

}  // namespace detail

}  // namespace baker
