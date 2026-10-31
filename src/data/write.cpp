#include <baker/write.hpp>

#include "common/containers.hpp"

#include "markers.hpp"
#include "text.hpp"

#include <charconv>
#include <cstdio>

namespace baker {
namespace {

char open_of(Type t) noexcept {
    return t == Type::Command ? '{' : t == Type::Property ? '[' : '(';
}
char close_of(Type t) noexcept {
    return t == Type::Command ? '}' : t == Type::Property ? ']' : ')';
}

// Where the parser reads a symbol specially: a layer verb's slot, where the verb reads back
// bare; the head of a parenthesized array, where a '/'-led one is a row's key; an
// operator at an odd slot of a command printed prefix, which must not read back as infix.
enum Slot : uint8_t { kPlain, kMarker, kHead, kQuoted };

void put_symbol(Str& out, std::string_view s, Slot slot = kPlain) {
    const bool key = slot == kHead && s.size() > 1 && s[0] == '/';
    if ((detail::is_bare_symbol(s) && !key && slot != kQuoted) || slot == kMarker) {
        out += s;
    } else {
        out += '\'';
        out += s;
        out += '\'';
    }
}

// What `a[i]` is to the parser: a verb, in a parenthesized array of the form the parser
// takes it in (elsewhere a verb's symbol is quoted, and reads back as that symbol); where it
// takes a row's key, the first.
Slot slot_of(const Node& n, const Array& a, uint32_t i,
             detail::InfixShape shape = detail::InfixShape::None) noexcept {
    if (shape == detail::InfixShape::Quoted && (i & 1) && detail::quoted_in_command(a[i]))
        return kQuoted;  // `{+ '-' 1}`
    if (n.type() != Type::Array) return kPlain;
    if (i == 1 && detail::verb_of(a) != detail::Marker::None) return kMarker;
    return i == 0 ? kHead : kPlain;
}

void put_atom(Str& out, const Node& n, Slot slot = kPlain) {
    char buf[64];
    switch (n.type()) {
        case Type::Int: {
            auto r = std::to_chars(buf, buf + sizeof buf, *n.try_int());
            out.append(buf, r.ptr);
            break;
        }
        case Type::Float: {
            const Str text = detail::format_double(*n.try_float());
            out += text;
            // Keep it a float on reparse: 5 must print as 5.0.
            if (text.find_first_of(".eEni") == Str::npos) out += ".0";
            break;
        }
        case Type::Symbol: put_symbol(out, n.try_sym()->str(), slot); break;
        case Type::String:
            out += '"';
            for (char c : n.string()->view()) {
                if (c == '"')
                    out += "\\q";
                else if (c == '\n')
                    out += "\\n";
                else
                    out += c;
            }
            out += '"';
            break;
        case Type::Var:
            out += '$';
            out += n.var_name().str();
            break;
        case Type::Func: put_symbol(out, n.func_record()->name.str()); break;
        case Type::Object: {
            const ObjectHandle h = n.object_handle();
            out +=
                "'<object " + detail::decimal(h.index()) + ':' + detail::decimal(h.salt()) + ">'";
            break;
        }
        case Type::Array:
        case Type::Command:
        case Type::Property: break;
    }
}

// How `n`, an array-like node, prints: a command by its infix shape; every other
// array head-first.
detail::InfixShape shape_of(const Node& n, const Array& a) noexcept {
    return n.type() == Type::Command ? detail::infix_shape(a) : detail::InfixShape::None;
}

// Compact form; stops once `out` exceeds `limit` bytes. Returns false if it stopped. An
// infix command prints `{k0 op k2 ...}`: its operator at slot 0 goes between the operands.
bool put_flat(Str& out, const Node& n, size_t limit, Slot slot = kPlain) {
    if (!n.is_array_like()) {
        put_atom(out, n, slot);
        return out.size() <= limit;
    }
    out += open_of(n.type());
    const Array& a = *n.array();
    const detail::InfixShape shape = shape_of(n, a);
    const uint32_t first = shape == detail::InfixShape::Chain ? 1 : 0;
    for (uint32_t i = first; i < a.size(); ++i) {
        if (i > first) {
            out += ' ';
            if (first) {
                out += a[0].try_sym()->str();
                out += ' ';
            }
        }
        if (!put_flat(out, a[i], limit, slot_of(n, a, i, shape))) return false;
    }
    out += close_of(n.type());
    return out.size() <= limit;
}

void put_pretty(Str& out, const Node& n, size_t indent, const WriteOptions& o, Slot slot = kPlain) {
    if (!n.is_array_like()) {
        put_atom(out, n, slot);
        return;
    }
    const size_t width = o.width > 0 ? static_cast<size_t>(o.width) : 0;
    const size_t budget = width > indent ? width - indent : 0;
    // The compact form if it fits, written once; else taken back.
    const size_t mark = out.size();
    if (put_flat(out, n, mark + budget, slot)) return;
    out.resize(mark);
    const size_t step = o.indent > 0 ? static_cast<size_t>(o.indent) : 0;
    const Array& a = *n.array();
    const detail::InfixShape shape = shape_of(n, a);
    const uint32_t first = shape == detail::InfixShape::Chain ? 1 : 0;
    out += open_of(n.type());
    // One child a line; an infix command's operator ends each line but the last.
    for (uint32_t i = first; i < a.size(); ++i) {
        if (i > first) {
            if (first) {
                out += ' ';
                out += a[0].try_sym()->str();
            }
            out += '\n';
            out.append(indent + step, ' ');
        }
        put_pretty(out, a[i], indent + step, o, slot_of(n, a, i, shape));
    }
    out += close_of(n.type());
}

}  // namespace

void write(const Node& node, TextSink text) {
    Str out;
    put_flat(out, node, SIZE_MAX);
    text(out);
}

void write(const Array& array, TextSink text) {
    Str out = "(";
    bool first = true;
    for (const Node& c : array) {
        if (!first) out += ' ';
        first = false;
        put_flat(out, c, SIZE_MAX);
    }
    out += ')';
    text(out);
}

namespace {
Str dta_text(const Array& root, const WriteOptions& options) {
    Str out;
    for (const Node& n : root) {
        put_pretty(out, n, 0, options);
        out += '\n';
    }
    // A reader skips a byte order mark at the very start of a file; a symbol that begins
    // with those bytes must not land there.
    if (out.starts_with("\xEF\xBB\xBF")) out.insert(0, " ");
    return out;
}
}  // namespace

void write_dta(const Array& root, TextSink out, const WriteOptions& options) {
    out(dta_text(root, options));
}

Expected<void> write_file(std::string_view path, const Array& root, const WriteOptions& options) {
    const Str text = dta_text(root, options);
    const Str p(path);
    std::FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return Unexpected(make_error(ErrorCode::Io, "cannot open '" + p + "' for writing"));
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) return Unexpected(make_error(ErrorCode::Io, "error writing '" + p + "'"));
    return {};
}

namespace detail {
Str preview(const Node& node, size_t max_chars) {
    Str out;
    if (!put_flat(out, node, max_chars) || out.size() > max_chars) {
        out.resize(max_chars > 3 ? max_chars - 3 : 0);
        out += "...";
    }
    return out;
}
}  // namespace detail

}  // namespace baker
