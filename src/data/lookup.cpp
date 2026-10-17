// Lookup: paths, sorted mode, and the error messages for failed lookups. The fast paths
// are inline in array.hpp; everything here except resolve() and find_sorted() runs only
// when a lookup has already failed.
#include <baker/array.hpp>
#include <baker/path.hpp>
#include <baker/write.hpp>

#include "common/containers.hpp"

#include "text.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <iterator>
#include <optional>

namespace baker {
namespace {

// The sorted-mode order: by hash, then text. Both depend only on the symbol's text, so
// the order is the same on every run.
bool tag_less(Symbol a, Symbol b) noexcept {
    if (a.hash() != b.hash()) return a.hash() < b.hash();
    return a != b && a.str() < b.str();
}

// A path segment as text: bare where it reads back as one, otherwise 'quoted'.
void append_name(Str& out, Symbol s) {
    detail::append_path_name(out, s.str());
}

using detail::quoted;

Str label_of(const Path::Segment& seg) {
    return seg.is_index ? "element " + detail::decimal(seg.index) : quoted(seg.name);
}

void locate(Error& e, const Array* a) {
    if (!a) return;
    e.set_location(a->file(), a->line());
}

bool ascii_iequal(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
               return lower(x) == lower(y);
           });
}

// "'x' not found", plus a hint for the two likeliest mistakes: the key was
// written as a "string", or with different capitalization.
Error not_found(Symbol tag, std::span<const Node> siblings) {
    Str msg = quoted(tag) + " not found";
    std::optional<Symbol> variant;
    for (const Node& n : siblings) {
        const Array* a = n.try_array();
        if (!a || a->empty()) continue;
        if (auto s = (*a)[0].try_str(); s && *s == tag.str()) {
            msg += "; \"" + Str(*s) + "\" at line " + detail::decimal(a->line()) +
                   " is a string, but keys are symbols: write " + quoted(tag);
            return make_error(ErrorCode::NotFound, std::move(msg));
        }
        if (auto s = (*a)[0].try_sym(); s && !variant && ascii_iequal(s->str(), tag.str()))
            variant = *s;
    }
    if (variant) msg += "; did you mean " + quoted(*variant) + "?";
    return make_error(ErrorCode::NotFound, std::move(msg));
}

Str preview_values(const Values& v) {
    Str out = "(";
    for (size_t i = 0; i < v.size() && i < 4; ++i) {
        if (i) out += ' ';
        out += detail::preview(v[i], 20);
    }
    if (v.size() > 4) out += " ...";
    return out + ")";
}

// `label` was found, but its values are not what `info` asked for.
Error conversion_error(const Str& label, const Values& v, const detail::ConvertInfo& info) {
    using detail::ConvertKind;
    if (v.empty()) return make_error(ErrorCode::Type, label + " has no value");
    const Node& n = v[0];
    const Str actual(type_name(n.type()));
    if (n.type() == Type::Command || n.type() == Type::Var)
        return make_error(ErrorCode::Type, label + " is a " + actual +
                                               "; reading it as a value needs baker::script");
    switch (info.kind) {
        case ConvertKind::Int:
        case ConvertKind::Float: {
            const std::string_view want = info.kind == ConvertKind::Int ? "int" : "float";
            if (!n.is_number())
                return make_error(ErrorCode::Type, label + " is " + actual + ", not " + Str(want));
            const double f = *n.try_float();
            if (info.kind == ConvertKind::Int && n.type() == Type::Float && std::trunc(f) != f)
                return make_error(ErrorCode::Type, label + " is float, not int");
            return make_error(ErrorCode::Range, label + " value " + detail::preview(n) +
                                                    " is out of range for " + Str(info.name));
        }
        case ConvertKind::Scalar:
            return make_error(ErrorCode::Type, label + " is " + actual + ", not " + Str(info.name));
        case ConvertKind::Compound: break;
    }
    return make_error(ErrorCode::Type,
                      label + " " + preview_values(v) + " does not read as " + Str(info.name));
}

// One path step from `v` (whose array is `arr`, or null for a leaf). False if missing.
bool step(Values& v, const Array*& arr, const Path::Segment& seg) noexcept {
    if (seg.is_index) {
        if (seg.index >= v.size()) return false;
        const Node& n = v[seg.index];
        if (const Array* a = n.try_array()) {
            v = Values(a->nodes(), a);
            arr = a;
        } else {
            v = Values(v.nodes().subspan(seg.index, 1), v.source());
            arr = nullptr;
        }
        return true;
    }
    // The array's children are the span plus, for a tagged array, the tag itself, which is
    // not an array; so searching the whole array is the same and can use sorted mode.
    const Array* a = arr ? arr->try_find(seg.name) : nullptr;
    if (!a) return false;
    v = a->values();
    arr = a;
    return true;
}

}  // namespace

// --- Path ---

Path::Path(std::string_view text) {
    if (!parse(text)) {
        valid_ = false;
        size_ = 0;
        bad_size_ = static_cast<uint32_t>(text.size() < kBadText ? text.size() : kBadText);
        text.copy(bad_text_, bad_size_);
    }
}

namespace {

// Reads the segment of `text` at `i` into `name` and moves `i` past it and the separator
// after it. False when it is malformed: an unclosed quote, an empty name, or a missing or
// trailing separator. Path::parse and Path::matches read a path through it alike.
bool next_segment(std::string_view text, size_t& i, std::string_view& name, bool& quoted) noexcept {
    const size_t n = text.size();
    quoted = text[i] == '\'';
    if (quoted) {
        const size_t close = text.find('\'', i + 1);
        if (close == std::string_view::npos) return false;
        name = text.substr(i + 1, close - i - 1);
        i = close + 1;
    } else {
        size_t j = i;
        while (j < n && text[j] != '.' && text[j] != '/') ++j;
        name = text.substr(i, j - i);
        if (name.empty()) return false;
        i = j;
    }
    if (i == n) return true;
    if (text[i] != '.' && text[i] != '/') return false;
    return ++i != n;
}

// An unquoted name of digits is an index.
bool is_index(std::string_view name, bool quoted) noexcept {
    return !quoted &&
           std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

bool Path::parse(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        std::string_view name;
        bool quoted = false;
        if (!next_segment(text, i, name, quoted)) return false;
        Segment seg;
        if (is_index(name, quoted)) {
            const auto r = std::from_chars(name.data(), name.data() + name.size(), seg.index);
            if (r.ec != std::errc()) return false;
            seg.is_index = true;
        } else {
            seg.name = Symbol(name);
        }
        if (size_ == kMaxSegments) return false;
        segments_[size_++] = seg;
    }
    return true;
}

void Path::write(TextSink out, size_t count, char separator) const {
    if (!valid_) {
        out(std::string_view(bad_text_, bad_size_));
        return;
    }
    Str text;
    for (size_t i = 0; i < size_ && i < count; ++i) {
        if (i) text += separator;
        if (segments_[i].is_index)
            text += detail::decimal(segments_[i].index);
        else
            append_name(text, segments_[i].name);
    }
    out(text);
}

bool Path::matches(std::string_view text) const noexcept {
    if (!valid_) return false;
    size_t k = 0;
    for (size_t i = 0; i < text.size();) {
        if (k == size_) return false;
        const Segment& seg = segments_[k++];
        std::string_view name;
        bool quoted = false;
        if (!next_segment(text, i, name, quoted)) return false;
        if (is_index(name, quoted)) {
            uint32_t index = 0;
            const auto r = std::from_chars(name.data(), name.data() + name.size(), index);
            if (r.ec != std::errc() || !seg.is_index || seg.index != index) return false;
        } else if (seg.is_index || seg.name.str() != name) {
            return false;
        }
    }
    return k == size_;
}

// --- Array lookup ---

Expected<const Array*> Array::find_array(Symbol tag) const {
    const Array* a = try_find(tag);
    if (a && a->size_ > 1)
        if (const Array* v = a->data()[1].try_array()) return v;
    return Unexpected(detail::find_error(*this, tag, a, &detail::kArrayInfo));
}

void Array::sort_for_lookup() {
    // A stable merge sort. std::stable_sort would take its buffer from operator new.
    const auto less = [](const Node& a, const Node& b) {
        const Symbol tb = detail::tag_of(b);
        if (tb.empty()) return false;
        const Symbol ta = detail::tag_of(a);
        return ta.empty() || tag_less(ta, tb);
    };
    Node* d = data();
    const size_t n = size_;
    Vec<Node> buf(n);
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * width) {
            const size_t mid = std::min(lo + width, n), hi = std::min(lo + 2 * width, n);
            std::merge(std::make_move_iterator(d + lo), std::make_move_iterator(d + mid),
                       std::make_move_iterator(d + mid), std::make_move_iterator(d + hi),
                       buf.begin() + static_cast<ptrdiff_t>(lo), less);
        }
        std::move(buf.begin(), buf.end(), d);
    }
    finish();  // the tag run follows the nodes
    flags_ = static_cast<uint16_t>(flags_ | kSortedForLookup);
}

const Array* Array::find_sorted(Symbol tag) const noexcept {
    if (tag.empty()) return nullptr;  // '' is no tag
    const Node* it = std::partition_point(begin(), end(), [tag](const Node& n) {
        const Symbol t = detail::tag_of(n);
        return t.empty() || tag_less(t, tag);
    });
    return it != end() && detail::tag_of(*it) == tag ? it->try_array() : nullptr;
}

namespace detail {

Error find_error(const Array& container, Symbol tag, const Array* found, const ConvertInfo* info,
                 const Values* values) {
    Error e = found && info
                  ? conversion_error(quoted(tag), values ? *values : found->values(), *info)
                  : not_found(tag, container.nodes());
    locate(e, found ? found : &container);
    if (!container.empty())
        if (auto t = container[0].try_sym()) {
            Str path(e.path());
            append_name(path, *t);
            e.set_path(path);
        }
    return e;
}

namespace {
std::atomic<Evaluator> g_evaluator{nullptr};
}  // namespace

void set_evaluator(Evaluator evaluator) noexcept {
    g_evaluator.store(evaluator, std::memory_order_release);
}

Evaluator evaluator() noexcept {
    return g_evaluator.load(std::memory_order_acquire);
}

Expected<bool> evaluate_values(const Values& in, Evaluated& out) {
    auto evaluable = [](const Node& n) {
        return n.type() == Type::Command || n.type() == Type::Var;
    };
    const Evaluator eval = evaluator();
    if (!eval || std::none_of(in.begin(), in.end(), evaluable)) return false;
    out.nodes.assign(in.nodes());
    for (Node& n : out.nodes.nodes()) {
        if (!evaluable(n)) continue;
        auto r = eval(n);
        if (!r) return Unexpected(std::move(r).error());
        n = std::move(*r);
    }
    out.values = Values(out.nodes.nodes(), in.source());
    return true;
}

ResolveStatus resolve(const Array& root, const Path& path, Resolved& out) noexcept {
    if (!path.valid()) return ResolveStatus::Malformed;
    Values v(root.nodes(), &root);
    const Array* arr = &root;
    for (size_t i = 0; i < path.size(); ++i)
        if (!step(v, arr, path[i])) return ResolveStatus::Missing;
    out = {v, arr};
    return ResolveStatus::Ok;
}

Error get_error(const Array& root, const Path& path, const ConvertInfo* info,
                const Values* values) {
    if (!path.valid()) {
        Error e = make_error(ErrorCode::Syntax, "malformed path '" + detail::text_of(path) + "'");
        locate(e, &root);
        return e;
    }
    Values v(root.nodes(), &root);
    const Array* arr = &root;
    for (size_t i = 0; i < path.size(); ++i) {
        const Values before = v;
        const Array* before_arr = arr;
        if (step(v, arr, path[i])) continue;
        const Path::Segment& seg = path[i];
        Error e =
            seg.is_index
                ? make_error(ErrorCode::NotFound,
                             label_of(seg) + " is out of range (" +
                                 detail::decimal(static_cast<int64_t>(before.size())) + " values)")
                : not_found(seg.name, before.nodes());
        locate(e, before_arr ? before_arr : before.source());
        e.set_path(detail::text_of(path, i));
        return e;
    }
    // The path resolved, so the value did not convert.
    const Str label = path.empty() ? "the array" : label_of(path[path.size() - 1]);
    Error e = conversion_error(label, values ? *values : v, info ? *info : kArrayInfo);
    locate(e, v.source());
    e.set_path(detail::text_of(path, path.empty() ? 0 : path.size() - 1));
    return e;
}

}  // namespace detail
}  // namespace baker
