#include "parse_harness.hpp"

#include <baker/array.hpp>
#include <baker/parse.hpp>
#include <baker/path.hpp>
#include <baker/std_interop.hpp>

#include <cstdint>
#include <vector>

namespace baker::fuzz {
namespace {

LoadSession make_session() {
    LoadSession s;
    s.set_reader([](std::string_view, TextSink) { return false; });
    s.limits.max_depth = 64;
    s.limits.max_nodes = 1u << 16;
    return s;
}

bool located(const Error& e) {
    return !e.message().empty() && e.file() == Symbol("lookup.dta") && e.line() >= 1;
}

// Sorted and linear find must agree on every tag among `a`'s children, and on `extra`.
bool sorted_agrees(const Array& a, Symbol extra) {
    auto sorted = Array::make_copy(a.nodes());
    sorted->sort_for_lookup();
    auto agrees = [&](Symbol tag) {
        const Array* l = a.try_find(tag);
        const Array* r = sorted->try_find(tag);
        return l == r;  // children are shared, so the same array or both null
    };
    if (!agrees(extra)) return false;
    for (const Array& child : a.subarrays())
        if (auto t = child.try_at(0) ? child[0].try_sym() : std::nullopt; t && !agrees(*t))
            return false;
    return true;
}

// Visits up to `budget` arrays of the tree, shared ones once per reference.
bool check_arrays(const Array& a, Symbol extra, uint64_t& budget) {
    if (budget == 0) return true;
    --budget;
    if (!sorted_agrees(a, extra)) return false;
    for (const Node& n : a)
        if (const Array* c = n.array())
            if (!check_arrays(*c, extra, budget)) return false;
    return true;
}

}  // namespace

bool check_lookup(std::string_view data) {
    const size_t nl = data.find('\n');
    const std::string_view path_text =
        data.substr(0, nl == std::string_view::npos ? data.size() : nl);
    const std::string_view text =
        nl == std::string_view::npos ? std::string_view() : data.substr(nl + 1);

    const Path path(path_text);
    if (path.valid()) {
        // The printed form reads back as the same path, unless a name holds a quote.
        bool quotes = false;
        for (size_t i = 0; i < path.size(); ++i)
            quotes |= !path[i].is_index && path[i].name.str().find('\'') != std::string_view::npos;
        const Path again(to_string(path));
        if (!quotes) {
            if (!again.valid() || again.size() != path.size()) return false;
            for (size_t i = 0; i < path.size(); ++i)
                if (again[i].is_index != path[i].is_index || again[i].index != path[i].index ||
                    again[i].name != path[i].name)
                    return false;
        }
    } else if (!path_text.starts_with(to_string(path))) {  // an invalid path keeps a prefix
        return false;
    }

    LoadSession s = make_session();
    auto root = read_string(text, "lookup.dta", s);
    if (!root) return true;  // check_parse covers parse errors
    const Array& r = **root;

    const Array* arr = r.try_get(path);
    auto as_node = r.get<Node>(path);
    auto as_float = r.get<double>(path);
    auto as_list = r.get<std::vector<Symbol>>(path);
    auto with_fallback = r.get<int64_t>(path, 7);
    for (const Error* e :
         {as_node ? nullptr : &as_node.error(), as_float ? nullptr : &as_float.error(),
          as_list ? nullptr : &as_list.error(), with_fallback ? nullptr : &with_fallback.error()})
        if (e && !located(*e)) return false;
    if (!path.valid() && (arr || as_node || with_fallback)) return false;
    if (arr && !as_list && as_list.error().code() == ErrorCode::NotFound) return false;

    const Symbol first =
        path.valid() && !path.empty() && !path[0].is_index ? path[0].name : Symbol("x");
    if (auto e = r.find_float(first); !e && !located(e.error())) return false;
    uint64_t budget = 2000;
    return check_arrays(r, first, budget);
}

}  // namespace baker::fuzz
