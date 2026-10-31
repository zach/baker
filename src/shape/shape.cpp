// baker::shape: shape declarations, checking, and enumeration.

#include <baker/shape.hpp>

#include <baker/curve.hpp>

#include "common/containers.hpp"
#include "data/markers.hpp"
#include "data/text.hpp"

#include <algorithm>
#include <atomic>
#include <optional>

#if defined(_MSC_VER)
#define BAKER_NOINLINE __declspec(noinline)
#define BAKER_COLD __declspec(noinline)
#else
#define BAKER_NOINLINE [[gnu::noinline]]
#define BAKER_COLD [[gnu::noinline, gnu::cold]]
#endif

namespace baker::shape {

namespace {

// The grammar's words. Its metadata carry the sigil, so a field may be named at, kind, desc
// or rule, and an unknown @word is an error instead of silence.
struct Names {
    baker::Symbol at{"@at"}, range{"@range"}, step{"@step"}, kind{"@kind"}, desc{"@desc"},
        note{"@note"}, optional{"@optional"}, list{"list"}, any_tag{"*"}, keyed{"@keyed"},
        one_of{"@one_of"}, key_of{"key_of"}, rule{"@rule"}, warn{"@warn"};
};
const Names& names() {
    static const Names n;
    return n;
}

using baker::detail::quoted;

// The built-in type a symbol names, or Shape: another shape's name.
Type type_named(baker::Symbol s) {
    static const std::pair<baker::Symbol, Type> builtins[] = {
        {"int", Type::Int},       {"float", Type::Float},   {"bool", Type::Bool},
        {"symbol", Type::Symbol}, {"string", Type::String}, {"any", Type::Any},
        {"curve", Type::Curve}};
    for (const auto& [name, t] : builtins)
        if (name == s) return t;
    return Type::Shape;
}

std::string_view type_text(Type t) {
    switch (t) {
        case Type::Int: return "int";
        case Type::Float: return "float";
        case Type::Bool: return "bool";
        case Type::Symbol: return "symbol";
        case Type::String: return "string";
        case Type::List: return "list";
        case Type::Shape: return "shape";
        case Type::Key: return "key";
        case Type::Any: return "any";
        case Type::Curve: return "curve";
    }
    return "any";
}

// The segments of a path written as symbols, each perhaps dotted: `bosses.*.phases` and
// `bosses * phases` both name a path.
void path_of(Values v, Vec<baker::Symbol>& out) {
    for (const Node& p : v) {
        const auto seg = p.try_sym();
        if (!seg) continue;
        std::string_view text = seg->str();
        for (size_t dot; (dot = text.find('.')) != std::string_view::npos;) {
            out.emplace_back(text.substr(0, dot));
            text.remove_prefix(dot + 1);
        }
        out.emplace_back(text);
    }
}

// (key_of TABLE): the table's path, or false if `a` is not that form.
bool key_of(const Array& a, List<baker::Symbol>& table) {
    if (a.size() < 2 || !a[0].is(names().key_of)) return false;
    for (uint32_t i = 1; i < a.size(); ++i)
        if (!a[i].try_sym()) return false;  // every segment a symbol
    Vec<baker::Symbol> path;
    path_of(a.values(), path);
    table = List<baker::Symbol>({path.data(), path.size()});
    return true;
}

}  // namespace

const Field* Shape::field(baker::Symbol field_name) const noexcept {
    for (const Field& f : fields)
        if (f.name == field_name) return &f;
    return nullptr;
}

struct Set::State {
    Vec<Shape> shapes;
    Vec<Vec<Field>> fields;  // parallel to shapes; Shape::fields is rebuilt from it
    Vec<Vec<Rule>> rules;    // likewise, Shape::rules
    Vec<Ref<Array>> keep;    // the declarations the fields' text points into
};

Set::Set() : st_(detail::create<State>(default_allocator(MemoryKind::Config))) {}
Set::~Set() {
    detail::destroy(default_allocator(MemoryKind::Config), st_);
}
Set::Set(const Set& other)
    : st_(detail::create<State>(default_allocator(MemoryKind::Config), *other.st_)) {}
Set& Set::operator=(const Set& other) {
    if (this != &other) *st_ = *other.st_;
    return *this;
}
Set::Set(Set&& other) noexcept : st_(detail::create<State>(default_allocator(MemoryKind::Config))) {
    std::swap(st_, other.st_);
}
Set& Set::operator=(Set&& other) noexcept {
    std::swap(st_, other.st_);
    return *this;
}

std::span<const Shape> Set::shapes() const noexcept {
    return {st_->shapes.data(), st_->shapes.size()};
}

const Shape* Set::find(baker::Symbol name) const noexcept {
    for (const Shape& s : st_->shapes)
        if (s.name == name) return &s;
    return nullptr;
}

namespace {

// Reads one field declaration: (name type (@range lo hi) (@step s) (@kind k) (@desc "")
// (@note "") (@optional) (@one_of a b ...)).
bool read_field(const Array& a, Field& f, Vec<Error>& errors) {
    auto bad = [&](std::string_view why) {
        errors.push_back(make_error(ErrorCode::Syntax, why, a.file(), a.line()));
        return false;
    };
    f.name = a[0].try_sym().value_or(baker::Symbol());
    f.file = a.file();
    f.line = a.line();
    if (a.size() < 2) return bad("field " + quoted(f.name) + " needs a type");
    f.shape = {};
    f.table = {};
    f.element = Type::Any;
    f.min_count = 0;
    f.max_count = UINT32_MAX;
    const Array* l = a[1].try_array();
    // (list T), (list T N) for exactly N values, (list T N M) for N to M.
    const bool list = l && l->size() >= 2 && l->size() <= 4 && (*l)[0].is(names().list);
    if (list && l->size() > 2) {
        const auto lo = (*l)[2].try_int();
        const auto hi = l->size() > 3 ? (*l)[3].try_int() : lo;
        if (!lo || !hi || *lo < 0 || *hi < *lo || *hi >= int64_t{UINT32_MAX})
            return bad("field " + quoted(f.name) +
                       " counts its list as (list T N) or (list T N M), 0 <= N <= M");
        f.min_count = static_cast<uint32_t>(*lo);
        f.max_count = static_cast<uint32_t>(*hi);
    }
    const Array* elem = list ? (*l)[1].try_array() : nullptr;
    if (const auto t = a[1].try_sym()) {
        f.type = type_named(*t);
        if (f.type == Type::Shape) f.shape = *t;
    } else if (l && key_of(*l, f.table)) {
        f.type = Type::Key;
    } else if (elem && key_of(*elem, f.table)) {
        f.type = Type::List;
        f.element = Type::Key;
    } else if (list && (*l)[1].try_sym()) {
        f.type = Type::List;
        f.element = type_named(*(*l)[1].try_sym());
        if (f.element == Type::Shape) f.shape = *(*l)[1].try_sym();
    } else {
        return bad("field " + quoted(f.name) +
                   " has a type that is not a symbol, (list T) or (key_of TABLE)");
    }
    const Names& n = names();
    for (uint32_t i = 2; i < a.size(); ++i) {
        const Array* m = a[i].try_array();
        const auto tag = m && !m->empty() ? (*m)[0].try_sym() : std::nullopt;
        if (!tag) return bad("field " + quoted(f.name) + " has metadata that is not (@tag ...)");
        if (!tag->is_meta())
            return bad("field " + quoted(f.name) + " has (" + Str(tag->str()) +
                       " ...) after its type, which is not metadata: write (@" + Str(tag->str()) +
                       " ...)");
        const Values v = m->values();
        if (*tag == n.range) {
            const auto lo = v.size() == 2 ? v[0].try_float() : std::nullopt;
            const auto hi = v.size() == 2 ? v[1].try_float() : std::nullopt;
            if (!lo || !hi || *lo > *hi)
                return bad("field " + quoted(f.name) + " needs (@range low high), low <= high");
            f.ranged = true;
            f.min = *lo;
            f.max = *hi;
        } else if (*tag == n.step) {
            f.step = v.size() == 1 ? v[0].try_float().value_or(0) : 0;
        } else if (*tag == n.kind) {
            f.kind = v.size() == 1 ? v[0].try_sym().value_or(baker::Symbol()) : baker::Symbol();
        } else if (*tag == n.desc) {
            f.desc = v.size() == 1 ? v[0].try_str().value_or("") : "";
        } else if (*tag == n.note) {
            f.note = v.size() == 1 ? v[0].try_str().value_or("") : "";
        } else if (*tag == n.optional) {
            f.optional = true;
        } else if (*tag == n.one_of) {
            const bool symbols =
                f.type == Type::Symbol || (f.type == Type::List && f.element == Type::Symbol);
            if (!symbols || v.empty())
                return bad("field " + quoted(f.name) +
                           " lists (@one_of ...) symbols, so its type is symbol or (list symbol)");
            // Declared again, a field adds what the new list names, in the order written.
            Vec<baker::Symbol> all(f.one_of.begin(), f.one_of.end());
            for (const Node& s : v) {
                const auto sym = s.try_sym();
                if (!sym) return bad("field " + quoted(f.name) + "'s (@one_of ...) holds symbols");
                if (std::find(all.begin(), all.end(), *sym) == all.end()) all.push_back(*sym);
            }
            f.one_of = List<baker::Symbol>({all.data(), all.size()});
        } else {
            return bad("field " + quoted(f.name) + " has unknown metadata " + quoted(*tag));
        }
    }
    return true;
}

}  // namespace

namespace {
// Whether `next`, a field declared again, loosens `held`: another type, a range or a list's
// count widened or dropped, a required field made optional, or a symbol added to its
// (@one_of ...).
bool loosens(const Field& held, const Field& next) noexcept {
    if (next.type != held.type || next.element != held.element || next.shape != held.shape ||
        !std::equal(next.table.begin(), next.table.end(), held.table.begin(), held.table.end()))
        return true;
    if (next.optional && !held.optional) return true;
    if (next.min_count < held.min_count || next.max_count > held.max_count) return true;
    if (!held.one_of.empty() && next.one_of.size() > held.one_of.size()) return true;
    return held.ranged && (!next.ranged || next.min < held.min || next.max > held.max);
}
// The rank of the pack `file` declares, or none: a file without a pack is unconstrained.
std::optional<uint32_t> rank_of(const LoadSession* packs, baker::Symbol file) noexcept {
    const Pack* p = packs ? packs->pack_of(file) : nullptr;
    return p ? std::optional<uint32_t>(p->rank) : std::nullopt;
}
}  // namespace

namespace {

// (@rule "message" {expression} (@warn)) into `r`; false, with a located error, if malformed.
bool read_rule(const Array& a, Rule& r, Vec<Error>& errors) {
    auto bad = [&](const Str& why) {
        errors.push_back(make_error(ErrorCode::Syntax, why, a.file(), a.line()));
        return false;
    };
    const auto message = a.size() >= 2 ? a[1].try_str() : std::nullopt;
    if (!message || message->empty() || a.size() < 3)
        return bad("a rule is declared as (@rule \"message\" {expression}), optionally (@warn)");
    if (a[2].type() != baker::Type::Command)
        return bad("rule \"" + Str(*message) + "\" holds " + detail::preview(a[2]) +
                   ", not an {expression}");
    r = Rule{*message, a[2], false, a.file(), a.line()};
    for (uint32_t i = 3; i < a.size(); ++i) {
        const Array* m = a[i].try_array();
        if (!m || m->size() != 1 || !(*m)[0].is(names().warn))
            return bad("rule \"" + Str(*message) + "\" has " + detail::preview(a[i]) +
                       " after its expression; only (@warn) may follow");
        r.warn = true;
    }
    return true;
}

// The first row in `a` that holds a layer verb, or null: one flag test for an array with none.
const Array* verb_in(const Array& a) {
    if (!a.has_marker()) return nullptr;
    if (detail::verb_of(a) != detail::Marker::None) return &a;
    for (const Node& n : a)
        if (const Array* c = n.try_array())
            if (const Array* found = verb_in(*c)) return found;
    return nullptr;
}

}  // namespace

void Set::add(const LoadSession& session, Report& report, baker::Symbol root) {
    Vec<Error> errors(report.errors.begin(), report.errors.end());
    Vec<Error> warnings(report.warnings.begin(), report.warnings.end());
    const Names& n = names();
    const LoadSession* packs = &session;
    for (const ShapeDecl& decl : session.shapes()) {
        if (!root.empty() && decl.root != root) continue;
        st_->keep.push_back(decl.form);
        {
            const Array* a = decl.form.get();
            const auto name = (*a)[0].try_sym();  // the parser took only a named form
            // A shape is declared whole: a verb would compose one past the one-way rule,
            // since each layer's shapes are read alone.
            if (const Array* v = verb_in(*a)) {
                errors.push_back(make_error(
                    ErrorCode::Syntax,
                    "shape '" + Str(name->str()) + "' holds " +
                        detail::preview(Node(Ref<Array>(const_cast<Array*>(v)))) +
                        ": a shape is declared whole, so a layer writes its fields, not verbs",
                    v->file(), v->line()));
                continue;
            }
            size_t i = 0;
            while (i < st_->shapes.size() && st_->shapes[i].name != *name) ++i;
            if (i == st_->shapes.size()) {
                st_->shapes.emplace_back();
                st_->fields.emplace_back();
                st_->rules.emplace_back();
                st_->shapes[i].name = *name;
            }
            Shape& s = st_->shapes[i];
            s.file = a->file();
            s.line = a->line();
            // Every child after the name: its fields, and its own metadata, which values()
            // leaves out.
            for (const Node& c : a->nodes().subspan(1)) {
                const Array* f = c.try_array();
                const auto tag = f && !f->empty() ? (*f)[0].try_sym() : std::nullopt;
                if (!tag) {
                    errors.push_back(make_error(
                        ErrorCode::Syntax,
                        "shape " + quoted(*name) + " has something other than a (field ...)",
                        a->file(), a->line()));
                    continue;
                }
                if (*tag == n.keyed) {
                    s.keyed = true;
                    continue;
                }
                if (*tag == n.at) {
                    Vec<baker::Symbol> path;
                    path_of(f->values(), path);
                    s.at = List<baker::Symbol>({path.data(), path.size()});
                    continue;
                }
                if (*tag == n.rule) {
                    // Declared again (the same message), a rule is replaced, as a field is;
                    // a softer pack may make a harder pack's rule an error, nothing more.
                    Rule rule;
                    if (!read_rule(*f, rule, errors)) continue;
                    Vec<Rule>& rs = st_->rules[i];
                    auto old = std::find_if(rs.begin(), rs.end(), [&](const Rule& x) {
                        return x.message == rule.message;
                    });
                    if (old == rs.end()) {
                        rs.push_back(rule);
                        continue;
                    }
                    const auto held = rank_of(packs, old->file);
                    const auto next = rank_of(packs, rule.file);
                    if (held && next && *next > *held &&
                        ((rule.warn && !old->warn) ||
                         detail::text_of(rule.expression) != detail::text_of(old->expression))) {
                        warnings.push_back(make_error(
                            ErrorCode::Refused,
                            "shape " + quoted(*name) + ": rule \"" + Str(rule.message) +
                                "\" is declared by a harder pack; a softer one may make it an "
                                "error, not soften or change it, so that declaration holds",
                            rule.file, rule.line));
                        continue;
                    }
                    *old = rule;
                    continue;
                }
                if (tag->is_meta()) {  // a shape's own (@desc ...) and (@note ...) describe it
                    if (*tag != n.desc && *tag != n.note)
                        errors.push_back(make_error(
                            ErrorCode::Syntax,
                            "shape " + quoted(*name) + " has unknown metadata " + quoted(*tag) +
                                ": (@at ...), (@keyed), (@rule ...), (@desc ...) or (@note ...)",
                            f->file(), f->line()));
                    continue;
                }
                // Declared again, a field takes the new type and whatever metadata the new
                // declaration gives, as a merge of the two would.
                Vec<Field>& fs = st_->fields[i];
                auto old = std::find_if(fs.begin(), fs.end(),
                                        [&](const Field& x) { return x.name == *tag; });
                Field field = old != fs.end() ? *old : Field{};
                if (!read_field(*f, field, errors)) continue;
                if (old != fs.end()) {
                    const auto held = rank_of(packs, old->file);
                    const auto next = rank_of(packs, field.file);
                    if (held && next && *next > *held && loosens(*old, field)) {
                        warnings.push_back(make_error(
                            ErrorCode::Refused,
                            "shape " + quoted(*name) + ": " + quoted(field.name) +
                                " is declared by a harder pack; a softer one may narrow it, "
                                "not loosen it, so that declaration holds",
                            field.file, field.line));
                        continue;
                    }
                    *old = field;
                } else {
                    fs.push_back(field);
                }
            }
        }
    }
    for (size_t i = 0; i < st_->shapes.size(); ++i) {
        Shape& s = st_->shapes[i];
        s.fields = List<Field>({st_->fields[i].data(), st_->fields[i].size()});
        s.rules = List<Rule>({st_->rules[i].data(), st_->rules[i].size()});
        if (s.keyed && s.at.empty())
            errors.push_back(make_error(
                ErrorCode::Syntax,
                "shape " + quoted(s.name) + " is (@keyed) but has no (@at ...): keyed under what?",
                s.file, s.line));
    }
    report.errors = List<Error>({errors.data(), errors.size()});
    report.warnings = List<Error>({warnings.data(), warnings.size()});
}

namespace {

// A key field's table as written, dotted; empty for any other field.
Str table_path(const Field& f) {
    Str out;
    for (const baker::Symbol seg : f.table) {
        if (!out.empty()) out += '.';
        out += seg.str();
    }
    return out;
}

struct Checker {
    const Set& set;
    Vec<Error>& errors;
    Vec<Error>& warnings;
    Vec<baker::Symbol> chain;   // the tags of the arrays from the root to here
    Vec<const Shape*> keyed;    // the (@keyed) shapes, which match by place, not by tag
    Vec<const Shape*> by_name;  // by Symbol::index(): the shape of that name
    // Arrays checked already as a field's value, below the instance being walked: a stack,
    // emptied as the walk leaves that instance.
    Vec<const Array*> nested;
    Vec<baker::Symbol> seen;  // the fields each instance on the stack has, from its mark
    // The arrays from the root to the record being checked, the root first: where a key's
    // table is looked up, outward from the record that refers to it. A field's own array is
    // not one: its children are its values.
    Vec<const Array*> arrays;
    const LoadSession* packs = nullptr;  // for the one-way rule over keys
    // Each table a key names, its keys indexed the first time: a check's lookups into one
    // table cost its size once, not once per lookup.
    HashMap<const Array*, HashMap<baker::Symbol, const Array*>> tables;
    bool keys = false;  // whether any field is a key: `arrays` is kept only then
    // For unused(): each shape of the set that governed an instance, by its index.
    Vec<bool>* used = nullptr;

    [[nodiscard]] const Shape* shape_named(baker::Symbol name) const noexcept {
        return name.index() < by_name.size() ? by_name[name.index()] : nullptr;
    }

    [[nodiscard]] Str path() const {
        Str p;
        for (const baker::Symbol s : chain) p += (p.empty() ? "" : "/") + Str(s.str());
        return p;
    }
    void report(Vec<Error>& to, ErrorCode code, std::string_view message, const Array& at) const {
        Error e = make_error(code, message, at.file(), at.line());
        e.set_path(path());
        to.push_back(std::move(e));
    }

    [[nodiscard]] bool governs(const Shape& s) const {
        if (s.at.empty()) return true;
        if (s.at.size() != chain.size() - 1) return false;  // the last is the array's own tag
        for (size_t i = 0; i < s.at.size(); ++i)
            if (s.at[i] != names().any_tag && s.at[i] != chain[i]) return false;
        return true;
    }

    // `a`'s fields, from `first`, against `s`.
    void instance(const Array& a, const Shape& s, uint32_t first) {
        if (used) (*used)[static_cast<size_t>(&s - set.shapes().data())] = true;
        const size_t mark = seen.size();
        for (uint32_t i = first; i < a.size(); ++i) {
            const Array* f = a[i].try_array();
            // '' is no tag.
            const auto tag = f && !f->tag().empty() ? std::optional(f->tag()) : std::nullopt;
            if (!tag) {
                report(warnings, ErrorCode::NotFound,
                       "a value that is not a field of shape " + quoted(s.name), a);
                continue;
            }
            chain.push_back(*tag);
            if (const Field* field = s.field(*tag)) {
                seen.push_back(*tag);
                value(*f, *field);
            } else {
                report(warnings, ErrorCode::NotFound,
                       quoted(*tag) + " is not a field of shape " + quoted(s.name) +
                           " (extra fields are allowed)",
                       *f);
            }
            chain.pop_back();
        }
        const auto mine = seen.begin() + static_cast<std::ptrdiff_t>(mark);
        for (const Field& field : s.fields)
            if (!field.optional && std::find(mine, seen.end(), field.name) == seen.end())
                report(errors, ErrorCode::NotFound,
                       quoted(s.name) + " has no " + quoted(field.name), a);
        seen.resize(mark);
    }

    // A nested shape's instance, `e`, from `first`: out of line, so that what it adds to the
    // ancestors for keys does not change how value() is compiled for the common fields.
    BAKER_NOINLINE void enter(const Array& e, const Shape& s, uint32_t first) {
        nested.push_back(&e);
        if (keys) arrays.push_back(&e);
        instance(e, s, first);
        if (keys) arrays.pop_back();
    }

    // One field's array: its values against the field's type and range.
    void value(const Array& f, const Field& field) {
        const Values v = f.values();
        if (field.type == Type::Any) return;
        if (field.type == Type::Shape) {
            if (const Shape* s = known(f, field)) {
                enter(f, *s, 1);
            }
            return;
        }
        if (field.type == Type::Curve) {
            curve(f, v, field);
            return;
        }
        if (field.type != Type::List && v.size() != 1) {
            report(errors, ErrorCode::Type,
                   quoted(field.name) + " takes one " + Str(type_text(field.type)) + ", given " +
                       detail::decimal(static_cast<int64_t>(v.size())) + " values",
                   f);
            return;
        }
        if (field.type == Type::List &&
            (v.size() < field.min_count || v.size() > field.max_count)) {
            count(f, v, field);
            return;
        }
        const Type t = field.type == Type::List ? field.element : field.type;
        for (const Node& n : v) {
            if (n.type() == baker::Type::Command || n.type() == baker::Type::Var ||
                n.type() == baker::Type::Property)
                continue;  // known only when run
            if (t == Type::Shape) {
                const Array* e = n.try_array();
                const Shape* s = known(f, field);
                if (!s) return;
                if (!e) {
                    report(errors, ErrorCode::Type,
                           quoted(field.name) + " holds " + quoted(s->name) + " arrays", f);
                    continue;
                }
                enter(*e, *s, e->tag().empty() ? 0 : 1);
                continue;
            }
            if (!fits(n, t)) {
                if (t == Type::Key) {  // checked against its table, not here
                    key(n, f, field);
                    continue;
                }
                report(errors, ErrorCode::Type,
                       quoted(field.name) + " is " + Str(type_name(n.type())) + ", not " +
                           Str(type_text(t)),
                       f);
                continue;
            }
            const auto x = n.try_float();
            if (field.ranged && x && (*x < field.min || *x > field.max))
                report(errors, ErrorCode::Range,
                       quoted(field.name) + " is " + detail::text_of(n) + ", outside " +
                           detail::text_of(Node(field.min)) + " to " +
                           detail::text_of(Node(field.max)),
                       f);
            if (!field.one_of.empty())
                if (const auto sym = n.try_sym(); sym && !one_of(field, *sym))
                    report(errors, ErrorCode::Range,
                           quoted(field.name) + " is " + quoted(*sym) + ", not one of " +
                               names_text(field.one_of),
                           f);
        }
    }

    // A list of the wrong count: "'pos' holds 2 values, not 3".
    BAKER_COLD void count(const Array& f, const Values& v, const Field& field) {
        const Str want =
            field.min_count == field.max_count
                ? detail::decimal(field.min_count)
                : detail::decimal(field.min_count) + " to " + detail::decimal(field.max_count);
        report(errors, ErrorCode::Range,
               quoted(field.name) + " holds " + detail::decimal(static_cast<int64_t>(v.size())) +
                   " values, not " + want,
               f);
    }

    // A curve field: its values a curve, and each point's y in the field's range.
    BAKER_COLD void curve(const Array& f, const Values& v, const Field& field) {
        Curve c;
        if (const CurveProblem p = read_curve(v, c)) {
            const Str what =
                p.at < v.size() ? detail::text_of(v[p.at]) + " " + Str(p.what) : Str(p.what);
            report(errors, ErrorCode::Type, quoted(field.name) + " is not a curve: " + what, f);
            return;
        }
        if (!field.ranged) return;
        for (size_t i = 0; i < c.size(); ++i) {
            const double y = c.point(i).y;
            if (y < field.min || y > field.max)
                report(errors, ErrorCode::Range,
                       quoted(field.name) + "'s point " +
                           detail::text_of(v[v.size() - c.size() + i]) + " has y outside " +
                           detail::text_of(Node(field.min)) + " to " +
                           detail::text_of(Node(field.max)),
                       f);
        }
    }

    // A key against its table: the nearest enclosing table of that path, the root last.
    BAKER_COLD void key(const Node& n, const Array& f, const Field& field) {
        const auto sym = n.try_sym();
        if (!sym) {
            report(errors, ErrorCode::Type,
                   quoted(field.name) + " is " + Str(type_name(n.type())) + ", not a key of " +
                       table_path(field),
                   f);
            return;
        }
        const Array* table = table_for(field);
        if (!table) {
            report(errors, ErrorCode::NotFound,
                   quoted(field.name) + " names a key of " + table_path(field) + ", and no table " +
                       table_path(field) + " encloses it",
                   f);
            return;
        }
        const Array* entry = in(*table, *sym);
        if (!entry) {
            Str message = quoted(*sym) + " is not a key of " + table_path(field);
            if (const auto near = nearest(*table, sym->str())) message += " (" + Str(*near) + "?)";
            report(errors, ErrorCode::NotFound, message, f);
            return;
        }
        // The one-way rule: a harder pack may not refer to a softer pack's key.
        const Pack* mine = packs ? packs->pack_of(f.file()) : nullptr;
        const Pack* theirs = mine ? packs->pack_of(entry->file()) : nullptr;
        if (theirs && theirs->rank > mine->rank)
            report(warnings, ErrorCode::Refused,
                   "pack '" + Str(mine->name.str()) + "' (level " + Str(mine->level.str()) +
                       ") names key " + quoted(*sym) + " of " + table_path(field) +
                       " from the softer pack '" + Str(theirs->name.str()) + "' (level " +
                       Str(theirs->level.str()) + ")",
                   f);
    }
    // The innermost array that holds the path's first segment, the root last, then the rest
    // of the path below it; none if nothing holds the first, or the rest is missing there.
    [[nodiscard]] const Array* table_for(const Field& field) const noexcept {
        for (size_t k = arrays.size(); k-- > 0;) {
            const Array* at = arrays[k]->try_find(field.table[0]);
            if (!at) continue;
            for (size_t i = 1; at && i < field.table.size(); ++i) at = at->try_find(field.table[i]);
            return at;
        }
        return nullptr;
    }
    // The table's child tagged `key`, the first as try_find() finds it.
    const Array* in(const Array& table, baker::Symbol key) {
        auto [it, added] = tables.try_emplace(&table);
        if (added)
            for (const Node& c : table.values())
                if (const Array* e = c.try_array(); e && !e->tag().empty())
                    it->second.try_emplace(e->tag(), e);
        const auto found = it->second.find(key);
        return found == it->second.end() ? nullptr : found->second;
    }
    // The table's key nearest `wrong` by edits, within two, or none: what a typo meant.
    static std::optional<std::string_view> nearest(const Array& table, std::string_view wrong) {
        std::optional<std::string_view> best;
        size_t best_cost = 3;
        for (const Node& c : table.values())
            if (const Array* e = c.try_array(); e && !e->tag().empty()) {
                const size_t cost = edits(wrong, e->tag().str(), best_cost);
                if (cost < best_cost) {
                    best_cost = cost;
                    best = e->tag().str();
                }
            }
        return best;
    }
    // The edit distance between `a` and `b`, or `cap` once it reaches it.
    static size_t edits(std::string_view a, std::string_view b, size_t cap) {
        if ((a.size() > b.size() ? a.size() - b.size() : b.size() - a.size()) >= cap) return cap;
        Vec<size_t> row(b.size() + 1);
        for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
        for (size_t i = 1; i <= a.size(); ++i) {
            size_t diag = row[0];
            row[0] = i;
            for (size_t j = 1; j <= b.size(); ++j) {
                const size_t up = row[j];
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] != b[j - 1])});
                diag = up;
            }
        }
        return std::min(row[b.size()], cap);
    }

    // A symbol against the field's list: pointer compares of interned symbols.
    static bool one_of(const Field& field, baker::Symbol s) noexcept {
        for (const baker::Symbol x : field.one_of)
            if (x == s) return true;
        return false;
    }
    static Str names_text(const List<baker::Symbol>& names) {
        Str out;
        for (const baker::Symbol x : names) {
            if (!out.empty()) out += ", ";
            out += x.str();
        }
        return out;
    }

    const Shape* known(const Array& f, const Field& field) {
        const Shape* s = shape_named(field.shape);
        if (!s)
            report(errors, ErrorCode::NotFound,
                   quoted(field.name) + " is of the unknown shape " + quoted(field.shape), f);
        return s;
    }

    static bool fits(const Node& n, Type t) {
        switch (t) {
            case Type::Int: return n.type() == baker::Type::Int;
            case Type::Float: return n.is_number();
            case Type::Bool: {
                bool b = false;
                return Values({&n, 1}, nullptr).read(0, b);
            }
            case Type::Symbol: return n.type() == baker::Type::Symbol;
            case Type::String: return n.type() == baker::Type::String;
            case Type::Key: return false;
            default: return true;
        }
    }

    // Keys: whether to keep `arrays`, a template argument so a set with no key field pays
    // nothing for it per array.
    template <bool Keys>
    void walk(const Array& a) {
        for (const Node& n : a) {
            const Array* c = n.array();
            if (!c) continue;
            const auto tag = !c->tag().empty() ? std::optional(c->tag()) : std::nullopt;
            if (tag) chain.push_back(*tag);
            if constexpr (Keys) arrays.push_back(c);
            const size_t mark = nested.size();
            if (tag && std::find(nested.begin(), nested.end(), c) == nested.end()) {
                const Shape* by_tag = shape_named(*tag);
                if (by_tag && !by_tag->keyed && governs(*by_tag)) instance(*c, *by_tag, 1);
                for (const Shape* s : keyed)
                    if (governs(*s)) instance(*c, *s, 1);
            }
            if (c->height() > 1) walk<Keys>(*c);
            nested.resize(mark);  // what this array's instance checked lies below it
            if constexpr (Keys) arrays.pop_back();
            if (tag) chain.pop_back();
        }
    }
};

}  // namespace

namespace {

// The records shapes with rules govern, found as Checker finds instances; a walk of its own
// so that Checker::walk, which every shape check runs, is compiled as it was.
struct RuleWalker {
    using Visit = Callback<void(const Array&, const Shape&, std::string_view)>;
    const Visit& visit;
    Vec<const Shape*> keyed, by_tag;  // shapes with rules, keyed at a path or by their tag
    Vec<baker::Symbol> chain;
    Str path;

    [[nodiscard]] bool governs(const Shape& s) const {
        if (s.at.empty()) return true;
        if (s.at.size() != chain.size() - 1) return false;
        for (size_t i = 0; i < s.at.size(); ++i)
            if (s.at[i] != names().any_tag && s.at[i] != chain[i]) return false;
        return true;
    }
    void walk(const Array& a) {
        for (const Node& n : a) {
            const Array* c = n.array();
            if (!c) continue;
            const auto tag = !c->tag().empty() ? std::optional(c->tag()) : std::nullopt;
            if (tag) {
                chain.push_back(*tag);
                path.clear();
                for (const baker::Symbol t : chain)
                    path += (path.empty() ? "" : "/") + Str(t.str());
                for (const Shape* s : by_tag)
                    if (s->name == *tag && governs(*s)) visit(*c, *s, path);
                for (const Shape* s : keyed)
                    if (governs(*s)) visit(*c, *s, path);
            }
            if (c->height() > 1) walk(*c);
            if (tag) chain.pop_back();
        }
    }
};

}  // namespace

void each_ruled(const Array& tree, const Set& shapes, const RuleWalker::Visit& visit) {
    RuleWalker w{visit, {}, {}, {}, {}};
    for (const Shape& s : shapes.shapes()) {
        if (s.rules.empty()) continue;
        (s.keyed && !s.at.empty() ? w.keyed : w.by_tag).push_back(&s);
    }
    if (!w.keyed.empty() || !w.by_tag.empty()) w.walk(tree);
}

namespace {
void run(const Array& tree, const Set& shapes, Vec<Error>& errors, Vec<Error>& warnings,
         const LoadSession* packs, Vec<bool>* used) {
    Checker checker{shapes, errors, warnings, {}, {}, {}, {}, {}, {}, packs, {}, false, used};
    checker.arrays.push_back(&tree);
    for (const Shape& s : shapes.shapes()) {
        if (s.keyed && !s.at.empty()) checker.keyed.push_back(&s);
        if (s.name.index() >= checker.by_name.size()) checker.by_name.resize(s.name.index() + 1);
        if (!checker.by_name[s.name.index()]) checker.by_name[s.name.index()] = &s;  // as find()
        for (const Field& f : s.fields)
            checker.keys |= f.type == Type::Key || f.element == Type::Key;
    }
    if (checker.keys)
        checker.walk<true>(tree);
    else
        checker.walk<false>(tree);
}
}  // namespace

void check(const Array& tree, const Set& shapes, Report& report, const LoadSession* packs) {
    Vec<Error> errors(report.errors.begin(), report.errors.end());
    Vec<Error> warnings(report.warnings.begin(), report.warnings.end());
    run(tree, shapes, errors, warnings, packs, nullptr);
    report.errors = List<Error>({errors.data(), errors.size()});
    report.warnings = List<Error>({warnings.data(), warnings.size()});
}

void unused(const Array& tree, const Set& shapes, Report& report) {
    Vec<Error> errors, warnings(report.warnings.begin(), report.warnings.end()), ignored;
    Vec<bool> used(shapes.shapes().size(), false);
    run(tree, shapes, errors, ignored, nullptr, &used);
    for (size_t i = 0; i < used.size(); ++i) {
        if (used[i]) continue;
        const Shape& s = shapes.shapes()[i];
        Str where = "no array is tagged " + quoted(s.name);
        if (!s.at.empty()) {
            Str at;
            for (const baker::Symbol t : s.at) at += (at.empty() ? "" : "/") + Str(t.str());
            where = s.keyed ? "no array is a child of " + at
                            : "no array tagged " + quoted(s.name) + " is in " + at;
        }
        warnings.push_back(make_error(ErrorCode::NotFound,
                                      "shape " + quoted(s.name) + " governs nothing: " + where,
                                      s.file, s.line));
    }
    report.warnings = List<Error>({warnings.data(), warnings.size()});
}

Config::Validator validator() {
    return [](const ValidatorStack& stack, const Callback<void(const Error&)>& warn) {
        Set set;
        Report report;
        for (const Ref<Array>& tree : stack.layers) set.add(stack.session, report, tree->file());
        check(stack.root, set, report, &stack.session);
        if (!warn) return;
        for (const Error& e : report.errors) warn(e);
        for (const Error& w : report.warnings) warn(w);
#if defined(BAKER_SHAPE_NO_SCRIPT)
        // Nothing in this build can evaluate a rule: say so once, not at every reload.
        static std::atomic<bool> told{false};
        size_t rules = 0;
        for (const Shape& s : set.shapes()) rules += s.rules.size();
        if (rules != 0 && !told.exchange(true))
            warn(make_error(ErrorCode::Refused,
                            "the shapes hold " + detail::decimal(static_cast<int64_t>(rules)) +
                                " rules, and this build has no baker::script to check them"));
#endif
    };
}

void write_json(const Set& shapes, TextSink text) {
    Vec<const Shape*> ss;
    for (const Shape& sh : shapes.shapes()) ss.push_back(&sh);
    std::sort(ss.begin(), ss.end(),
              [](const Shape* a, const Shape* b) { return a->name.str() < b->name.str(); });
    Str out = "{\n  \"schema\": \"baker.shapes/1.5\",\n  \"shapes\": [";
    for (size_t i = 0; i < ss.size(); ++i) {
        const Shape& sh = *ss[i];
        out += i ? ",\n    {\"name\": " : "\n    {\"name\": ";
        detail::json_string(out, sh.name.str());
        out += ", \"at\": ";
        detail::json_names(out, sh.at.items());
        out += Str(", \"keyed\": ") + (sh.keyed ? "true" : "false") + ", \"file\": ";
        detail::json_string(out, sh.file.str());
        out += ", \"line\": " + detail::decimal(sh.line) + ", \"fields\": [";
        for (size_t j = 0; j < sh.fields.size(); ++j) {
            const Field& f = sh.fields[j];
            out += j ? ",\n      {\"name\": " : "\n      {\"name\": ";
            detail::json_string(out, f.name.str());
            out += ", \"type\": ";
            detail::json_string(out, type_text(f.type));
            out += ", \"element\": ";
            detail::json_string(out, f.type == Type::List ? type_text(f.element) : "");
            out += ", \"count\": ";
            if (f.type == Type::List && (f.min_count != 0 || f.max_count != UINT32_MAX))
                out +=
                    "[" + detail::decimal(f.min_count) + ", " + detail::decimal(f.max_count) + "]";
            else
                out += "null";
            out += ", \"shape\": ";
            detail::json_string(out, f.shape.str());
            out += ", \"table\": ";
            detail::json_string(out, table_path(f));
            out += Str(", \"optional\": ") + (f.optional ? "true" : "false") + ", \"range\": ";
            if (f.ranged)
                out +=
                    "[" + detail::text_of(Node(f.min)) + ", " + detail::text_of(Node(f.max)) + "]";
            else
                out += "null";
            out += ", \"step\": " + detail::text_of(Node(f.step)) + ", \"kind\": ";
            detail::json_string(out, f.kind.str());
            out += ", \"desc\": ";
            detail::json_string(out, f.desc);
            out += ", \"note\": ";
            detail::json_string(out, f.note);
            out += ", \"one_of\": ";
            detail::json_names(out, f.one_of.items());
            out += ", \"file\": ";
            detail::json_string(out, f.file.str());
            out += ", \"line\": " + detail::decimal(f.line) + "}";
        }
        out += sh.fields.empty() ? "], \"rules\": [" : "\n    ], \"rules\": [";
        for (size_t j = 0; j < sh.rules.size(); ++j) {
            const Rule& r = sh.rules[j];
            out += j ? ",\n      {\"message\": " : "\n      {\"message\": ";
            detail::json_string(out, r.message);
            out += ", \"expression\": ";
            detail::json_string(out, detail::text_of(r.expression));
            out += Str(", \"warn\": ") + (r.warn ? "true" : "false") + ", \"file\": ";
            detail::json_string(out, r.file.str());
            out += ", \"line\": " + detail::decimal(r.line) + "}";
        }
        out += sh.rules.empty() ? "]}" : "\n    ]}";
    }
    out += ss.empty() ? "]\n}\n" : "\n  ]\n}\n";
    text(out);
}

}  // namespace baker::shape
