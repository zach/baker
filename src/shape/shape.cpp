// baker::shape: shape declarations, checking, and enumeration.

#include <baker/shape.hpp>

#include "common/containers.hpp"
#include "data/text.hpp"

#include <algorithm>
#include <optional>

namespace baker::shape {

namespace {

struct Names {
    baker::Symbol shapes{"shapes"}, at{"at"}, range{"range"}, step{"step"}, kind{"kind"},
        desc{"desc"}, note{"note"}, optional{"optional"}, list{"list"}, any_tag{"*"},
        keyed{"keyed"}, one_of{"one_of"};
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
        {"symbol", Type::Symbol}, {"string", Type::String}, {"any", Type::Any}};
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
        case Type::Any: return "any";
    }
    return "any";
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
    Vec<Ref<Array>> keep;    // the declarations the fields' text points into
};

Set::Set() : st_(detail::create<State>(default_allocator())) {}
Set::~Set() {
    detail::destroy(default_allocator(), st_);
}
Set::Set(const Set& other) : st_(detail::create<State>(default_allocator(), *other.st_)) {}
Set& Set::operator=(const Set& other) {
    if (this != &other) *st_ = *other.st_;
    return *this;
}
Set::Set(Set&& other) noexcept : st_(detail::create<State>(default_allocator())) {
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

// Reads one field declaration: (name type (range lo hi) (step s) (kind k) (desc "")
// (note "") (optional) (one_of a b ...)).
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
    f.element = Type::Any;
    if (const auto t = a[1].try_sym()) {
        f.type = type_named(*t);
        if (f.type == Type::Shape) f.shape = *t;
    } else if (const Array* l = a[1].try_array();
               l && l->size() == 2 && (*l)[0].is(names().list) && (*l)[1].try_sym()) {
        f.type = Type::List;
        f.element = type_named(*(*l)[1].try_sym());
        if (f.element == Type::Shape) f.shape = *(*l)[1].try_sym();
    } else {
        return bad("field " + quoted(f.name) + " has a type that is not a symbol or (list T)");
    }
    const Names& n = names();
    for (uint32_t i = 2; i < a.size(); ++i) {
        const Array* m = a[i].try_array();
        const auto tag = m && !m->empty() ? (*m)[0].try_sym() : std::nullopt;
        if (!tag) return bad("field " + quoted(f.name) + " has metadata that is not (tag ...)");
        const Values v = m->values();
        if (*tag == n.range) {
            const auto lo = v.size() == 2 ? v[0].try_float() : std::nullopt;
            const auto hi = v.size() == 2 ? v[1].try_float() : std::nullopt;
            if (!lo || !hi || *lo > *hi)
                return bad("field " + quoted(f.name) + " needs (range low high), low <= high");
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
                           " lists (one_of ...) symbols, so its type is symbol or (list symbol)");
            // Declared again, a field adds what the new list names, in the order written.
            Vec<baker::Symbol> all(f.one_of.begin(), f.one_of.end());
            for (const Node& s : v) {
                const auto sym = s.try_sym();
                if (!sym) return bad("field " + quoted(f.name) + "'s (one_of ...) holds symbols");
                if (std::find(all.begin(), all.end(), *sym) == all.end()) all.push_back(*sym);
            }
            f.one_of = List<baker::Symbol>({all.data(), all.size()});
        }  // other metadata is the tools' business
    }
    return true;
}

}  // namespace

namespace {
// Whether `next`, a field declared again, loosens `held`: another type, a range widened or
// dropped, a required field made optional, or a symbol added to its (one_of ...).
bool loosens(const Field& held, const Field& next) noexcept {
    if (next.type != held.type || next.element != held.element || next.shape != held.shape)
        return true;
    if (next.optional && !held.optional) return true;
    if (!held.one_of.empty() && next.one_of.size() > held.one_of.size()) return true;
    return held.ranged && (!next.ranged || next.min < held.min || next.max > held.max);
}
// The rank of the pack `file` declares, or none: a file without a pack is unconstrained.
std::optional<uint32_t> rank_of(const LoadSession* packs, baker::Symbol file) noexcept {
    const Pack* p = packs ? packs->pack_of(file) : nullptr;
    return p ? std::optional<uint32_t>(p->rank) : std::nullopt;
}
}  // namespace

void Set::add(const Array& tree, Report& report, const LoadSession* packs) {
    Vec<Error> errors(report.errors.begin(), report.errors.end());
    Vec<Error> warnings(report.warnings.begin(), report.warnings.end());
    const Names& n = names();
    for (const Node& top : tree) {
        const Array* decls = top.try_array();
        if (!decls || decls->empty() || !(*decls)[0].is(n.shapes)) continue;
        st_->keep.emplace_back(const_cast<Array*>(decls));
        for (const Node& d : decls->values()) {
            const Array* a = d.try_array();
            const auto name = a && !a->empty() ? (*a)[0].try_sym() : std::nullopt;
            if (!name) {
                const Array& at = a ? *a : *decls;
                errors.push_back(make_error(ErrorCode::Syntax,
                                            "a shape is declared as (name field ...)", at.file(),
                                            at.line()));
                continue;
            }
            size_t i = 0;
            while (i < st_->shapes.size() && st_->shapes[i].name != *name) ++i;
            if (i == st_->shapes.size()) {
                st_->shapes.emplace_back();
                st_->fields.emplace_back();
                st_->shapes[i].name = *name;
            }
            Shape& s = st_->shapes[i];
            s.file = a->file();
            s.line = a->line();
            for (const Node& c : a->values()) {
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
                    for (const Node& p : f->values()) {
                        const auto seg = p.try_sym();
                        if (!seg) continue;
                        // `bosses.*.phases` and `bosses * phases` both name a path.
                        std::string_view text = seg->str();
                        for (size_t dot; (dot = text.find('.')) != std::string_view::npos;) {
                            path.emplace_back(text.substr(0, dot));
                            text.remove_prefix(dot + 1);
                        }
                        path.emplace_back(text);
                    }
                    s.at = List<baker::Symbol>({path.data(), path.size()});
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
        if (s.keyed && s.at.empty())
            errors.push_back(make_error(
                ErrorCode::Syntax,
                "shape " + quoted(s.name) + " is (keyed) but has no (at ...): keyed under what?",
                s.file, s.line));
    }
    report.errors = List<Error>({errors.data(), errors.size()});
    report.warnings = List<Error>({warnings.data(), warnings.size()});
}

namespace {

struct Checker {
    const Set& set;
    Vec<Error>& errors;
    Vec<Error>& warnings;
    Vec<baker::Symbol> chain;   // the tags of the arrays from the root to here
    Vec<const Shape*> keyed;    // the (keyed) shapes, which match by place, not by tag
    Vec<const Shape*> by_name;  // by Symbol::index(): the shape of that name
    // Arrays checked already as a field's value, below the instance being walked: a stack,
    // emptied as the walk leaves that instance.
    Vec<const Array*> nested;
    Vec<baker::Symbol> seen;  // the fields each instance on the stack has, from its mark

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

    // One field's array: its values against the field's type and range.
    void value(const Array& f, const Field& field) {
        const Values v = f.values();
        if (field.type == Type::Any) return;
        if (field.type == Type::Shape) {
            if (const Shape* s = known(f, field)) {
                nested.push_back(&f);
                instance(f, *s, 1);
            }
            return;
        }
        if (field.type != Type::List && v.size() != 1) {
            report(errors, ErrorCode::Type,
                   quoted(field.name) + " takes one " + Str(type_text(field.type)) + ", given " +
                       detail::decimal(static_cast<int64_t>(v.size())) + " values",
                   f);
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
                nested.push_back(e);
                instance(*e, *s, e->tag().empty() ? 0 : 1);
                continue;
            }
            if (!fits(n, t)) {
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
            default: return true;
        }
    }

    void walk(const Array& a) {
        for (const Node& n : a) {
            const Array* c = n.array();
            if (!c) continue;
            const auto tag = !c->tag().empty() ? std::optional(c->tag()) : std::nullopt;
            if (tag && chain.empty() && *tag == names().shapes) continue;  // declarations
            if (tag) chain.push_back(*tag);
            const size_t mark = nested.size();
            if (tag && std::find(nested.begin(), nested.end(), c) == nested.end()) {
                const Shape* by_tag = shape_named(*tag);
                if (by_tag && !by_tag->keyed && governs(*by_tag)) instance(*c, *by_tag, 1);
                for (const Shape* s : keyed)
                    if (governs(*s)) instance(*c, *s, 1);
            }
            if (c->height() > 1) walk(*c);
            nested.resize(mark);  // what this array's instance checked lies below it
            if (tag) chain.pop_back();
        }
    }
};

}  // namespace

void check(const Array& tree, const Set& shapes, Report& report) {
    Vec<Error> errors(report.errors.begin(), report.errors.end());
    Vec<Error> warnings(report.warnings.begin(), report.warnings.end());
    Checker checker{shapes, errors, warnings, {}, {}, {}, {}, {}};
    for (const Shape& s : shapes.shapes()) {
        if (s.keyed && !s.at.empty()) checker.keyed.push_back(&s);
        if (s.name.index() >= checker.by_name.size()) checker.by_name.resize(s.name.index() + 1);
        if (!checker.by_name[s.name.index()]) checker.by_name[s.name.index()] = &s;  // as find()
    }
    checker.walk(tree);
    report.errors = List<Error>({errors.data(), errors.size()});
    report.warnings = List<Error>({warnings.data(), warnings.size()});
}

Config::Validator validator() {
    return [](const ValidatorStack& stack, const Callback<void(const Error&)>& warn) {
        Set set;
        Report report;
        for (const Ref<Array>& tree : stack.layers) set.add(*tree, report, &stack.session);
        check(stack.root, set, report);
        if (!warn) return;
        for (const Error& e : report.errors) warn(e);
        for (const Error& w : report.warnings) warn(w);
    };
}

void write_json(const Set& shapes, TextSink text) {
    Vec<const Shape*> ss;
    for (const Shape& sh : shapes.shapes()) ss.push_back(&sh);
    std::sort(ss.begin(), ss.end(),
              [](const Shape* a, const Shape* b) { return a->name.str() < b->name.str(); });
    Str out = "{\n  \"schema\": \"baker.shapes/1.1\",\n  \"shapes\": [";
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
            out += ", \"shape\": ";
            detail::json_string(out, f.shape.str());
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
        out += sh.fields.empty() ? "]}" : "\n    ]}";
    }
    out += ss.empty() ? "]\n}\n" : "\n  ]\n}\n";
    text(out);
}

}  // namespace baker::shape
