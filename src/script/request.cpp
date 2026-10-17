// eval_request: one line of a tool's request, one line of JSON in reply: the schema
// baker.reply/1.1, which docs/reply-json.md describes.

#include <baker/parse.hpp>
#include <baker/script.hpp>
#include <baker/write.hpp>

#include "common/containers.hpp"
#include "data/text.hpp"

#include <cmath>

namespace baker {
namespace {

using detail::json_string;

void json_value(Str& out, const Node& n);

// An array's children from `first` as a JSON array.
void json_array(Str& out, const Array& a, size_t first) {
    out += '[';
    for (size_t i = first; i < a.size(); ++i) {
        if (i > first) out += ',';
        json_value(out, a[i]);
    }
    out += ']';
}

// The tag of a child that is an array headed by a symbol; empty otherwise.
Symbol child_tag(const Node& n) {
    if (n.type() != Type::Array) return {};
    const Array* a = n.array();
    if (a->empty()) return {};
    const auto tag = (*a)[0].try_sym();
    return tag ? *tag : Symbol();
}

// A keyed array: after an optional leading symbol, every child an array headed by a
// distinct symbol. It reads as a JSON object of those children.
bool keyed(const Array& a, size_t first) {
    if (first >= a.size()) return false;
    for (size_t i = first; i < a.size(); ++i) {
        const Symbol t = child_tag(a[i]);
        if (t.empty()) return false;
        for (size_t j = first; j < i; ++j)
            if (child_tag(a[j]) == t) return false;
    }
    return true;
}

void json_value(Str& out, const Node& n) {
    switch (n.type()) {
        case Type::Int: out += detail::decimal(*n.try_int()); return;
        case Type::Float: {
            const double d = *n.try_float();
            if (!std::isfinite(d)) {
                out += "null";
                return;
            }
            out += detail::text_of(n);  // the printer's shortest form is valid JSON
            return;
        }
        case Type::Symbol: json_string(out, n.try_sym()->str()); return;
        case Type::String: json_string(out, *n.try_str()); return;
        case Type::Array: {
            const Array& a = *n.array();
            const size_t first = !a.empty() && a[0].type() == Type::Symbol ? 1 : 0;
            if (!keyed(a, first)) {
                json_array(out, a, 0);
                return;
            }
            out += '{';
            for (size_t i = first; i < a.size(); ++i) {
                const Array& c = *a[i].array();
                if (i > first) out += ',';
                json_string(out, c[0].try_sym()->str());
                out += ':';
                if (keyed(c, 1))
                    json_value(out, a[i]);  // (box (width 2) (height 1)): an object
                else if (c.size() == 1)
                    out += "null";
                else if (c.size() == 2)
                    json_value(out, c[1]);
                else
                    json_array(out, c, 1);
            }
            out += '}';
            return;
        }
        default: json_string(out, detail::text_of(n)); return;  // $var, a function, an object
    }
}

void json_error(Str& out, const Error& e) {
    out += "{\"code\":";
    json_string(out, error_code_name(e.code()));
    out += ",\"file\":";
    json_string(out, e.file().str());
    out += ",\"line\":" + detail::decimal(e.line()) + ",\"column\":" + detail::decimal(e.column()) +
           ",\"path\":";
    json_string(out, e.path());
    out += ",\"message\":";
    json_string(out, e.message());
    out += ",\"stack\":[";
    const auto stack = e.stack();
    for (size_t i = 0; i < stack.size(); ++i) {
        out += i ? ",{\"function\":" : "{\"function\":";
        json_string(out, stack[i].function.str());
        out += ",\"on\":";
        json_string(out, stack[i].on.str());
        out += ",\"file\":";
        json_string(out, stack[i].file.str());
        out += ",\"line\":" + detail::decimal(stack[i].line) + "}";
    }
    out += "]}";
}

// A request is (ID {command}) with ID an int, symbol or string.
bool is_request(const Array& tree) {
    if (tree.size() != 1 || tree[0].type() != Type::Array) return false;
    const Array& r = *tree[0].array();
    if (r.size() != 2 || r[1].type() != Type::Command) return false;
    const Type id = r[0].type();
    return id == Type::Int || id == Type::Symbol || id == Type::String;
}

}  // namespace

void eval_request(Context& ctx, std::string_view line, TextSink reply) {
    Str out = R"({"schema":"baker.reply/1.1","id":)";
    LoadSession session;
    session.allocator = &ctx.allocator();  // the tree is the Context's, as eval(text) builds it
    auto tree = read_string(line, "<console>", session);
    Expected<Node> result = Node();
    if (!tree) {
        out += "null";
        result = Unexpected(std::move(tree).error());
    } else if (is_request(**tree)) {
        const Array& r = *(**tree)[0].array();
        json_value(out, r[0]);
        result = ctx.eval(r[1]);
    } else {
        out += "null";
        result = ctx.run((*tree)->nodes());
    }
    if (result) {
        out += R"(,"ok":true,"text":)";
        json_string(out, detail::text_of(*result));
        out += ",\"value\":";
        json_value(out, *result);
    } else {
        out += R"(,"ok":false,"error":)";
        json_error(out, result.error());
    }
    out += "}\n";
    reply(out);
}

}  // namespace baker
