// Generating views (<baker/shape.hpp>): the C++ a shape marked (@view) becomes. The
// text is what tests/shape/views/ holds as golden files, and what the data tests read
// through; a change here changes them in the same commit.
#include <baker/shape.hpp>

#include "common/containers.hpp"
#include "data/text.hpp"

#include <algorithm>
#include <cstdint>

namespace baker::shape {

namespace {

using baker::detail::quoted;

void append_hex16(Str& out, uint64_t v) {
    static constexpr char digits[] = "0123456789abcdef";
    char buf[16];
    for (int i = 15; i >= 0; --i, v >>= 4) buf[i] = digits[v & 0xF];
    out += std::string_view(buf, 16);
}

bool identifier(std::string_view s) {
    if (s.empty()) return false;
    const auto word = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    if (!word(s[0])) return false;
    for (const char c : s)
        if (!word(c) && !(c >= '0' && c <= '9')) return false;
    return true;
}

bool keyword(std::string_view s) {
    static constexpr std::string_view kKeywords[] = {
        "alignas",       "alignof",     "and",
        "and_eq",        "asm",         "auto",
        "bitand",        "bitor",       "bool",
        "break",         "case",        "catch",
        "char",          "char8_t",     "char16_t",
        "char32_t",      "class",       "compl",
        "concept",       "const",       "consteval",
        "constexpr",     "constinit",   "const_cast",
        "continue",      "co_await",    "co_return",
        "co_yield",      "decltype",    "default",
        "delete",        "do",          "double",
        "dynamic_cast",  "else",        "enum",
        "explicit",      "export",      "extern",
        "false",         "float",       "for",
        "friend",        "goto",        "if",
        "inline",        "int",         "long",
        "mutable",       "namespace",   "new",
        "noexcept",      "not",         "not_eq",
        "nullptr",       "operator",    "or",
        "or_eq",         "private",     "protected",
        "public",        "register",    "reinterpret_cast",
        "requires",      "return",      "short",
        "signed",        "sizeof",      "static",
        "static_assert", "static_cast", "struct",
        "switch",        "template",    "this",
        "thread_local",  "throw",       "true",
        "try",           "typedef",     "typeid",
        "typename",      "union",       "unsigned",
        "using",         "virtual",     "void",
        "volatile",      "wchar_t",     "while",
        "xor",           "xor_eq"};
    return std::find(std::begin(kKeywords), std::end(kKeywords), s) != std::end(kKeywords);
}

// boss_phase -> BossPhase: each part between underscores, its first letter upper-cased.
Str pascal_case(std::string_view name) {
    Str out;
    bool upper = true;
    for (const char c : name) {
        if (c == '_') {
            upper = true;
            continue;
        }
        out += upper && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        upper = false;
    }
    return out;
}

Str struct_name(const Shape& s) {
    return s.cxx_name.empty() ? pascal_case(s.name.str()) : Str(s.cxx_name.str());
}

Str member_name(const Field& f) {
    return f.cxx_name.empty() ? Str(f.name.str()) : Str(f.cxx_name.str());
}

// The C++ type of a scalar member, by the shape type and its (@cxx T).
Str scalar_type(Type t, baker::Symbol cxx, const Set& set, baker::Symbol shape,
                const ViewOptions& o) {
    switch (t) {
        case Type::Int: {
            const std::string_view c = cxx.empty() ? "int32" : cxx.str();
            return Str(c) + "_t";
        }
        case Type::Float: return cxx.empty() ? "float" : Str(cxx.str());
        case Type::Bool: return "bool";
        case Type::Symbol:
        case Type::Key: return "baker::Symbol";
        case Type::String:
        case Type::Asset: return "std::string_view";
        case Type::Curve: return "baker::Curve";
        case Type::Shape: {
            const Shape* nested = set.find(shape);
            return nested ? Str(o.ns) + "::" + struct_name(*nested) : Str("void");
        }
        case Type::List:
        case Type::Any: return "void";
    }
    return "void";
}

Str member_type(const Field& f, const Set& set, const ViewOptions& o) {
    if (f.type == Type::List)
        return "baker::ListView<" + scalar_type(f.element, {}, set, f.shape, o) + ">";
    return scalar_type(f.type, f.cxx, set, f.shape, o);
}

// The name the reader passes as the member's C++ type, for its range error.
Str cxx_name_text(const Field& f) {
    if (!f.cxx.empty()) return Str(f.cxx.str());
    return f.type == Type::Int ? "int32" : "float";
}

bool in_view(const Field& f) {
    return f.view;
}

// Generated text is laid out by the generator, not by clang-format: a project that formats
// its build tree, or checks in a golden copy, leaves it alone.
void header_comment(Str& out, const Shape& s) {
    out += "// Generated by baker_bake --views from #shape (" + Str(s.name.str()) + " ...) in " +
           Str(s.file.str()) + ": do not edit.\n// clang-format off\n#pragma once\n\n";
}

// The shapes `s` nests by value or in a list, each once, sorted by name.
void nested_shapes(const Shape& s, Vec<baker::Symbol>& out) {
    for (const Field& f : s.fields) {
        if (!in_view(f) || f.shape.empty()) continue;
        if (f.type != Type::Shape && !(f.type == Type::List && f.element == Type::Shape)) continue;
        if (std::find(out.begin(), out.end(), f.shape) == out.end()) out.push_back(f.shape);
    }
    std::sort(out.begin(), out.end(),
              [](baker::Symbol a, baker::Symbol b) { return a.str() < b.str(); });
}

}  // namespace

List<baker::Symbol> view_shapes(const Set& set, std::span<const baker::Symbol> listed,
                                Report& report) {
    Vec<Error> errors(report.errors.begin(), report.errors.end());
    Vec<baker::Symbol> names;
    const auto add = [&](baker::Symbol name) {
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    };
    for (const Shape& s : set.shapes())
        if (s.view) add(s.name);
    for (const baker::Symbol name : listed) {
        if (set.find(name))
            add(name);
        else
            errors.push_back(make_error(ErrorCode::NotFound,
                                        "the project lists a view of shape " + quoted(name) +
                                            ", which no contract layer declares"));
    }
    // Nested shapes, transitively; a shape that reaches itself is an error.
    for (size_t i = 0; i < names.size(); ++i) {
        const Shape* s = set.find(names[i]);
        Vec<baker::Symbol> nested;
        nested_shapes(*s, nested);
        for (const baker::Symbol n : nested) {
            if (!set.find(n)) {
                errors.push_back(
                    make_error(ErrorCode::NotFound,
                               "shape " + quoted(s->name) + " nests the unknown shape " + quoted(n),
                               s->file, s->line));
                continue;
            }
            add(n);
        }
    }
    // Cycles: walk each view's nesting, depth first.
    for (const baker::Symbol name : names) {
        Vec<baker::Symbol> stack{name};
        Vec<baker::Symbol> path;
        // An iterative DFS over nested shapes, with `path` the chain from `name`.
        struct Frame {
            const Shape* shape;
            Vec<baker::Symbol> nested;
            size_t next = 0;
        };
        Vec<Frame> frames;
        Frame first{set.find(name), {}, 0};
        nested_shapes(*first.shape, first.nested);
        frames.push_back(std::move(first));
        path.push_back(name);
        bool cycle = false;
        while (!frames.empty() && !cycle) {
            Frame& top = frames.back();
            if (top.next == top.nested.size()) {
                frames.pop_back();
                path.pop_back();
                continue;
            }
            const baker::Symbol n = top.nested[top.next++];
            if (std::find(path.begin(), path.end(), n) != path.end()) {
                if (n == name) cycle = true;
                continue;
            }
            const Shape* ns = set.find(n);
            if (!ns) continue;
            Frame f{ns, {}, 0};
            nested_shapes(*ns, f.nested);
            frames.push_back(std::move(f));
            path.push_back(n);
        }
        if (cycle) {
            const Shape* s = set.find(name);
            errors.push_back(make_error(
                ErrorCode::Refused,
                "shape " + quoted(name) + " nests itself, so its view cannot be a struct", s->file,
                s->line));
        }
    }
    std::sort(names.begin(), names.end(),
              [](baker::Symbol a, baker::Symbol b) { return a.str() < b.str(); });
    // Names and fields.
    Vec<Str> struct_names;
    for (const baker::Symbol name : names) {
        const Shape& s = *set.find(name);
        const Str sn = struct_name(s);
        if (!identifier(sn) || keyword(sn))
            errors.push_back(make_error(ErrorCode::Refused,
                                        "shape " + quoted(s.name) + " would generate `" + sn +
                                            "`, which is not a C++ identifier; name it with "
                                            "(@cxx_name N)",
                                        s.file, s.line));
        for (size_t i = 0; i < struct_names.size(); ++i)
            if (struct_names[i] == sn)
                errors.push_back(make_error(ErrorCode::Refused,
                                            "shapes " + quoted(names[i]) + " and " +
                                                quoted(s.name) + " both generate `" + sn +
                                                "`; name one with (@cxx_name N)",
                                            s.file, s.line));
        struct_names.push_back(sn);
        Vec<Str> members;
        uint32_t required = 0;
        for (const Field& f : s.fields) {
            if (!in_view(f)) continue;
            if (f.type == Type::Any || (f.type == Type::List && f.element == Type::Any))
                errors.push_back(make_error(ErrorCode::Refused,
                                            "field " + quoted(f.name) + " of shape " +
                                                quoted(s.name) +
                                                " is `any`, which no struct can hold: mark it "
                                                "(@view no) to read it from the tree",
                                            f.file, f.line));
            const Str mn = member_name(f);
            if (!identifier(mn) || keyword(mn))
                errors.push_back(make_error(ErrorCode::Refused,
                                            "field " + quoted(f.name) + " of shape " +
                                                quoted(s.name) + " would generate the member `" +
                                                mn +
                                                "`, which is not a C++ identifier; name it with "
                                                "(@cxx_name N)",
                                            f.file, f.line));
            if (std::find(members.begin(), members.end(), mn) != members.end())
                errors.push_back(make_error(ErrorCode::Refused,
                                            "field " + quoted(f.name) + " of shape " +
                                                quoted(s.name) + " generates the member `" + mn +
                                                "`, which another field does too",
                                            f.file, f.line));
            members.push_back(mn);
            if (!f.optional) ++required;
        }
        if (required > 64)
            errors.push_back(make_error(ErrorCode::Limit,
                                        "shape " + quoted(s.name) + " has " +
                                            detail::decimal(required) +
                                            " required fields; a view holds at most 64",
                                        s.file, s.line));
    }
    report.errors = List<Error>({errors.data(), errors.size()});
    return List<baker::Symbol>({names.data(), names.size()});
}

void write_view_header(const Set& set, baker::Symbol shape, const ViewOptions& o, TextSink out) {
    const Shape* sp = set.find(shape);
    if (!sp) return;
    const Shape& s = *sp;
    Str text;
    header_comment(text, s);
    text += "#include <baker/view.hpp>\n";
    bool curve = false;
    for (const Field& f : s.fields)
        if (in_view(f) &&
            (f.type == Type::Curve || (f.type == Type::List && f.element == Type::Curve)))
            curve = true;
    if (curve) text += "#include <baker/curve.hpp>\n";
    Vec<baker::Symbol> nested;
    nested_shapes(s, nested);
    for (const baker::Symbol n : nested) text += "#include \"" + Str(n.str()) + ".hpp\"\n";
    text +=
        "\n#include <cstdint>\n#include <span>\n#include "
        "<string_view>\n\nnamespace " +
        Str(o.ns) + " {\n\n";
    const Str sn = struct_name(s);
    text += "struct " + sn + " {\n";
    for (const Field& f : s.fields) {
        if (!in_view(f)) continue;
        text += "    " + member_type(f, set, o) + " " + member_name(f);
        if (f.type == Type::Int || f.type == Type::Float)
            text += " = 0";
        else if (f.type == Type::Bool)
            text += " = false";
        text += ";\n";
    }
    text += "\n    static constexpr std::string_view kShapeName = baker::view_text(\"" +
            Str(s.name.str()) + "\");\n    static constexpr uint64_t kShapeDigest = 0x";
    append_hex16(text, view_digest(set, shape));
    // Where the shape's records are, and the members a table relocates.
    Str at;
    for (const baker::Symbol seg : s.at) at += (at.empty() ? "" : " ") + Str(seg.str());
    text += "ull;\n    static constexpr std::string_view kAt = \"" + at +
            "\";\n    static constexpr bool kKeyed = " + (s.keyed ? "true" : "false") +
            ";\n    // The members a baked table relocates: defined in " + Str(s.name.str()) +
            "_read.hpp.\n    static constexpr std::span<const baker::ViewFixup> fixups() "
            "noexcept;\n};\n\n}  // namespace " +
            Str(o.ns) + "\n";
    out(text);
}

// The fix-up list of `s`, as its reader header defines it: the members that point
// outside the struct, and, for a nested view, its own list and reader.
Str fixups_text(const Set& set, const Shape& s, const ViewOptions& o) {
    const Str sn = Str(o.ns) + "::" + struct_name(s);
    Vec<Str> fixups;
    for (const Field& f : s.fields) {
        if (!in_view(f)) continue;
        const Str offset = "offsetof(" + sn + ", " + member_name(f) + ")";
        Str nested_fixups = "nullptr, 0", nested_read = "nullptr";
        if (const Shape* n = f.shape.empty() ? nullptr : set.find(f.shape)) {
            const Str nn = Str(o.ns) + "::" + struct_name(*n);
            nested_fixups =
                nn + "::fixups().data(), static_cast<uint32_t>(" + nn + "::fixups().size())";
            nested_read = "&baker::view_detail::read_erased<" + nn +
                          ">, &baker::view_detail::init_erased<" + nn + ">";
        }
        // Every member of the fix-up spelled, so no build warns of a missing initializer.
        const Str none = ", baker::ViewFixup::Element::Int32, 0, nullptr, 0, nullptr, nullptr}";
        if (f.type == Type::Symbol || f.type == Type::Key) {
            fixups.push_back("{" + offset + ", baker::ViewFixupKind::Symbol" + none);
        } else if (f.type == Type::String || f.type == Type::Asset) {
            fixups.push_back("{" + offset + ", baker::ViewFixupKind::String" + none);
        } else if (f.type == Type::Curve) {
            fixups.push_back("{" + offset + ", baker::ViewFixupKind::Curve" + none);
        } else if (f.type == Type::Shape) {
            fixups.push_back(
                "{" + offset +
                ", baker::ViewFixupKind::Struct, baker::ViewFixup::Element::Struct, 0, " +
                nested_fixups + ", " + nested_read + "}");
        } else if (f.type == Type::List) {
            const char* element = "Int32";
            switch (f.element) {
                case Type::Float: element = "Float"; break;
                case Type::Bool: element = "Bool"; break;
                case Type::Symbol:
                case Type::Key: element = "Symbol"; break;
                case Type::String:
                case Type::Asset: element = "String"; break;
                case Type::Curve: element = "Curve"; break;
                case Type::Shape: element = "Struct"; break;
                default: break;
            }
            const bool nested = f.element == Type::Shape;
            fixups.push_back("{" + offset + ", baker::ViewFixupKind::List, " +
                             "baker::ViewFixup::Element::" + element + ", sizeof(" +
                             scalar_type(f.element, {}, set, f.shape, o) + "), " +
                             (nested ? nested_fixups : Str("nullptr, 0")) + ", " +
                             (nested ? nested_read : Str("nullptr, nullptr")) + "}");
        }
    }
    Str text = "\nnamespace " + Str(o.ns) + " {\n\n";
    if (fixups.empty()) {
        text += "constexpr std::span<const baker::ViewFixup> " + sn +
                "::fixups() noexcept {\n    return {};\n}\n";
    } else {
        text += "inline constexpr baker::ViewFixup k" + struct_name(s) + "Fixups[] = {\n";
        for (const Str& f : fixups) text += "    " + f + ",\n";
        text += "};\nconstexpr std::span<const baker::ViewFixup> " + sn +
                "::fixups() noexcept {\n    return k" + struct_name(s) + "Fixups;\n}\n";
    }
    text += "\n}  // namespace " + Str(o.ns) + "\n";
    return text;
}

void write_view_reader(const Set& set, baker::Symbol shape, const ViewOptions& o, TextSink out) {
    const Shape* sp = set.find(shape);
    if (!sp) return;
    const Shape& s = *sp;
    const Str sn = Str(o.ns) + "::" + struct_name(s);
    const Str name = Str(s.name.str());
    Str text;
    header_comment(text, s);
    text += "#include \"" + name + ".hpp\"\n";
    Vec<baker::Symbol> nested;
    nested_shapes(s, nested);
    for (const baker::Symbol n : nested) text += "#include \"" + Str(n.str()) + "_read.hpp\"\n";
    text +=
        "#include <baker/view_read.hpp>\n\n#include <cstddef>\n\nnamespace baker {\n\ntemplate "
        "<>\nstruct ViewReader<" +
        sn + "> {\n";
    // The required fields, in order: a bit each in `seen`.
    Vec<const Field*> required;
    for (const Field& f : s.fields)
        if (in_view(f) && !f.optional) required.push_back(&f);
    if (!required.empty()) {
        text += "    static constexpr std::string_view kRequired[] = {";
        for (size_t i = 0; i < required.size(); ++i)
            text += (i ? ", \"" : "\"") + Str(required[i]->name.str()) + "\"";
        text += "};\n";
    }
    text +=
        "    static bool read(const Array& record, " + sn +
        "& out, Error* err) noexcept {\n        using namespace baker::literals;\n"
        "        bool failed = false;  // the first failed field's error is kept; the rest read\n";
    if (!required.empty()) text += "        uint64_t seen = 0;\n";
    text +=
        "        for (const Node& n : record.nodes().subspan(1)) {\n"
        "            const Array* f = n.try_array();\n"
        "            if (!f) continue;\n"
        "            const Symbol tag = f->tag();\n"
        "            switch (tag.hash()) {\n";
    size_t bit = 0;
    for (const Field& f : s.fields) {
        if (!in_view(f)) continue;
        const Str fname = Str(f.name.str());
        const Str mem = "out." + member_name(f);
        // An old name the shape renamed reads as the field: a case of its own.
        text += "                case \"" + fname + "\"_hash:\n";
        Str spelled = "tag.str() != \"" + fname + "\"";
        for (const baker::Symbol old : f.renamed_from) {
            text += "                case \"" + Str(old.str()) + "\"_hash:\n";
            spelled += " && tag.str() != \"" + Str(old.str()) + "\"";
        }
        text += "                    if (" + spelled + ") break;\n                    if (!";
        switch (f.type) {
            case Type::Int:
                text += "view_detail::read_int(*f, " + mem + ", err, \"" + name + "\", \"" +
                        cxx_name_text(f) + "\")";
                break;
            case Type::Float:
                text += "view_detail::read_float(*f, " + mem + ", err, \"" + name + "\", \"" +
                        cxx_name_text(f) + "\")";
                break;
            case Type::Bool:
                text += "view_detail::read_bool(*f, " + mem + ", err, \"" + name + "\")";
                break;
            case Type::Symbol:
            case Type::Key:
                text += "view_detail::read_symbol(*f, " + mem + ", err, \"" + name + "\")";
                break;
            case Type::String:
            case Type::Asset:
                text += "view_detail::read_string(*f, " + mem + ", err, \"" + name + "\")";
                break;
            case Type::Curve:
                text += "view_detail::read_curve(*f, " + mem + ", err, \"" + name + "\")";
                break;
            case Type::Shape: text += "view_detail::read_nested(*f, " + mem + ", err)"; break;
            case Type::List:
                text +=
                    "view_detail::read_list(*f, " + mem + ", err, \"" + name + "\", " +
                    detail::decimal(f.min_count) + ", " +
                    (f.max_count == UINT32_MAX ? Str("UINT32_MAX") : detail::decimal(f.max_count)) +
                    ")";
                break;
            case Type::Any: text += "false"; break;
        }
        text +=
            ") {\n                        failed = true;\n                        err = nullptr;\n"
            "                    }\n";
        if (!f.optional) {
            text += "                    seen |= uint64_t{1} << " +
                    detail::decimal(static_cast<int64_t>(bit)) + ";\n";
            ++bit;
        }
        text += "                    break;\n";
    }
    text += "                default: break;\n            }\n        }\n";
    if (!required.empty()) {
        Str all =
            "(uint64_t{1} << " + detail::decimal(static_cast<int64_t>(required.size())) + ") - 1";
        if (required.size() == 64) all = "UINT64_MAX";
        text += "        if (seen != " + all +
                ") {\n            if (err) *err = view_detail::missing(record, \"" + name +
                "\", view_detail::first_missing(seen, kRequired));\n            return false;\n    "
                "    }\n";
    }
    text += "        return !failed;\n    }\n};\n\ntemplate <>\nstruct Convert<" + sn +
            "> {\n    static constexpr std::string_view name = \"" + name +
            "\";\n    static bool from(const Values& v, " + sn +
            "& out) noexcept {\n        return v.source() && ViewReader<" + sn +
            ">::read(*v.source(), out, nullptr);\n    }\n};\n\n}  // namespace baker\n";
    text += fixups_text(set, s, o);
    out(text);
}

void write_views_umbrella(const Set& set, std::span<const baker::Symbol> views,
                          const ViewOptions& o, TextSink out) {
    Str text =
        "// Generated by baker_bake --views: every view, and kViews for check_views. Do not edit.\n"
        "// clang-format off\n#pragma once\n\n";
    for (const baker::Symbol v : views) text += "#include \"" + Str(v.str()) + ".hpp\"\n";
    text += "#include <baker/view.hpp>\n\n#include <span>\n\nnamespace " + Str(o.ns) + " {\n\n";
    if (views.empty()) {
        text += "inline constexpr std::span<const baker::ViewInfo> kViews{};\n";
    } else {
        text += "inline constexpr baker::ViewInfo kViews[] = {\n";
        for (const baker::Symbol v : views) {
            const Shape* s = set.find(v);
            if (!s) continue;
            const Str sn = struct_name(*s);
            text += "    {" + sn + "::kShapeName, " + sn + "::kShapeDigest},\n";
        }
        text += "};\n";
    }
    text += "\n}  // namespace " + Str(o.ns) + "\n";
    out(text);
}

void write_tables_header(const Set& set, std::span<const baker::Symbol> views, const ViewOptions& o,
                         TextSink out) {
    Str text =
        "// Generated by baker_bake --views: register_tables, a baked table for every keyed "
        "view. Do not edit.\n// clang-format off\n#pragma once\n\n";
    Vec<const Shape*> keyed;
    for (const baker::Symbol v : views)
        if (const Shape* s = set.find(v); s && s->keyed) keyed.push_back(s);
    for (const Shape* s : keyed) text += "#include \"" + Str(s->name.str()) + "_read.hpp\"\n";
    text +=
        "#include <baker/table.hpp>\n\nnamespace " + Str(o.ns) +
        " {\n\n// Registers every keyed view's table with `cfg`, before its load; the first "
        "refusal, if any.\ninline baker::Expected<void> register_tables(baker::Config& cfg) {\n";
    for (const Shape* s : keyed)
        text += "    if (auto r = baker::add_table<" + Str(o.ns) + "::" + struct_name(*s) +
                ">(cfg); !r) return r;\n";
    if (keyed.empty()) text += "    (void)cfg;\n";
    text += "    return {};\n}\n\n}  // namespace " + Str(o.ns) + "\n";
    out(text);
}

void check_views(const Set& set, std::span<const ViewInfo> compiled, Report& report) {
    Vec<Error> warnings(report.warnings.begin(), report.warnings.end());
    const auto hex = [](uint64_t v) {
        Str s = "0x";
        append_hex16(s, v);
        return s;
    };
    for (const ViewInfo& v : compiled) {
        if (v.name.empty()) {  // a shipping build: by digest alone
            bool found = false;
            for (const Shape& s : set.shapes())
                if (view_digest(set, s.name) == v.digest) found = true;
            if (!found)
                warnings.push_back(make_error(
                    ErrorCode::Refused, "a compiled view (digest " + hex(v.digest) +
                                            ") matches no shape's layout: regenerate the views "
                                            "(baker_bake --views)"));
            continue;
        }
        const baker::Symbol name(v.name);
        const Shape* s = set.find(name);
        if (!s) {
            warnings.push_back(
                make_error(ErrorCode::NotFound, "view " + quoted(name) +
                                                    " names a shape no contract layer declares: "
                                                    "regenerate the views (baker_bake --views)"));
            continue;
        }
        const uint64_t now = view_digest(set, name);
        if (now != v.digest)
            warnings.push_back(make_error(
                ErrorCode::Refused,
                "view " + quoted(name) +
                    " was generated from another layout of its shape (compiled " + hex(v.digest) +
                    ", declared " + hex(now) + "): regenerate the views (baker_bake --views)",
                s->file, s->line));
    }
    report.warnings = List<Error>({warnings.data(), warnings.size()});
}

List<baker::Symbol> contract_roots(const Config& cfg,
                                   std::span<const baker::Symbol> contract_layers) {
    Vec<baker::Symbol> roots;
    const auto files = cfg.stack_files();
    for (const baker::Symbol layer : contract_layers) {
        // A file layer: its own file. A directory: every file the stack read under it.
        if (const Ref<Array> root = cfg.layer_root(layer.str()); root && !root->file().empty()) {
            roots.push_back(root->file());
            continue;
        }
        for (const StackFile& f : files) {
            if (f.layer != layer || !f.directory) continue;
            const std::string_view dir = f.path.str();
            for (const StackFile& g : files)
                if (!g.directory && g.present && g.path.str().size() > dir.size() &&
                    g.path.str().substr(0, dir.size()) == dir &&
                    (g.path.str()[dir.size()] == '/' || g.path.str()[dir.size()] == '\\'))
                    roots.push_back(g.path);
        }
    }
    return List<baker::Symbol>({roots.data(), roots.size()});
}

}  // namespace baker::shape
