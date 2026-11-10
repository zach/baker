// baker::shape's tool code: what a tool asks of a stack's shapes (the keys and assets its
// records name, the shapes that govern nothing) and the shapes as JSON. Apart from shape.cpp
// so that a game linking only the validator does not carry it.

#include <baker/shape.hpp>

#include "common/containers.hpp"
#include "data/text.hpp"
#include "shape/impl.hpp"

#include <algorithm>

namespace baker::shape {

List<AssetUse> asset_uses(const Array& tree, const Set& shapes) {
    Vec<Error> errors, warnings;
    Vec<AssetUse> uses;
    impl::run(tree, shapes, errors, warnings, nullptr, nullptr, nullptr, nullptr, nullptr, &uses);
    return List<AssetUse>({uses.data(), uses.size()});
}

List<KeyUse> key_uses(const Array& tree, const Set& shapes) {
    Vec<Error> errors, warnings;
    Vec<KeyUse> uses;
    impl::run(tree, shapes, errors, warnings, nullptr, nullptr, nullptr, nullptr, &uses);
    return List<KeyUse>({uses.data(), uses.size()});
}

void unused(const Array& tree, const Set& shapes, Report& report) {
    Vec<Error> errors, warnings(report.warnings.begin(), report.warnings.end()), ignored;
    Vec<bool> used(shapes.shapes().size(), false);
    impl::run(tree, shapes, errors, ignored, nullptr, &used);
    for (size_t i = 0; i < used.size(); ++i) {
        if (used[i]) continue;
        const Shape& s = shapes.shapes()[i];
        Str where = "no array is tagged " + baker::detail::quoted(s.name);
        if (!s.at.empty()) {
            Str at;
            for (const baker::Symbol t : s.at) at += (at.empty() ? "" : "/") + Str(t.str());
            where = s.keyed ? "no array is a child of " + at
                            : "no array tagged " + baker::detail::quoted(s.name) + " is in " + at;
        }
        warnings.push_back(
            make_error(ErrorCode::NotFound,
                       "shape " + baker::detail::quoted(s.name) + " governs nothing: " + where,
                       s.file, s.line));
    }
    report.warnings = List<Error>({warnings.data(), warnings.size()});
}

void write_json(const Set& shapes, TextSink text) {
    Vec<const Shape*> ss;
    for (const Shape& sh : shapes.shapes()) ss.push_back(&sh);
    std::sort(ss.begin(), ss.end(),
              [](const Shape* a, const Shape* b) { return a->name.str() < b->name.str(); });
    Str out = "{\n  \"schema\": \"baker.shapes/1.8\",\n  \"shapes\": [";
    for (size_t i = 0; i < ss.size(); ++i) {
        const Shape& sh = *ss[i];
        out += i ? ",\n    {\"name\": " : "\n    {\"name\": ";
        detail::json_string(out, sh.name.str());
        out += ", \"at\": ";
        detail::json_names(out, sh.at.items());
        out += Str(", \"keyed\": ") + (sh.keyed ? "true" : "false") +
               ", \"view\": " + (sh.view ? "true" : "false") + ", \"cxx_name\": ";
        detail::json_string(out, sh.cxx_name.str());
        out += ", \"file\": ";
        detail::json_string(out, sh.file.str());
        out += ", \"line\": " + detail::decimal(sh.line) + ", \"fields\": [";
        for (size_t j = 0; j < sh.fields.size(); ++j) {
            const Field& f = sh.fields[j];
            out += j ? ",\n      {\"name\": " : "\n      {\"name\": ";
            detail::json_string(out, f.name.str());
            out += ", \"type\": ";
            detail::json_string(out, impl::type_text(f.type));
            out += ", \"element\": ";
            detail::json_string(out, f.type == Type::List ? impl::type_text(f.element) : "");
            out += ", \"count\": ";
            if (f.type == Type::List && (f.min_count != 0 || f.max_count != UINT32_MAX))
                out +=
                    "[" + detail::decimal(f.min_count) + ", " + detail::decimal(f.max_count) + "]";
            else
                out += "null";
            out += ", \"shape\": ";
            detail::json_string(out, f.shape.str());
            out += ", \"table\": ";
            detail::json_string(out, impl::table_path(f));
            out += ", \"asset\": ";
            detail::json_string(out, f.asset.str());
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
            out += Str(", \"view\": ") + (f.view ? "true" : "false") + ", \"cxx\": ";
            detail::json_string(out, f.cxx.str());
            out += ", \"cxx_name\": ";
            detail::json_string(out, f.cxx_name.str());
            out += ", \"renamed_from\": ";
            detail::json_names(out, f.renamed_from.items());
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
