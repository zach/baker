#pragma once

// Opt-in interop with the C++ standard library, for hosts that use it.
//
// Baker's other public headers reach only language-support headers and the non-allocating
// vocabulary types (string_view, span, optional, expected), so an
// engine that bans the STL can include them. This header is the exception, by name: it
// includes the standard containers, and nothing else in Baker includes it.

#include <baker/array.hpp>
#include <baker/error.hpp>
#include <baker/list.hpp>
#include <baker/path.hpp>
#include <baker/symbol.hpp>
#include <baker/text.hpp>
#include <baker/write.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

template <>
struct std::hash<baker::Symbol> {
    size_t operator()(baker::Symbol s) const noexcept { return static_cast<size_t>(s.hash()); }
};

namespace baker {

// A TextSink that appends to `s`.
inline TextSink string_out(std::string& s) {
    return {[](void* u, std::string_view t) { static_cast<std::string*>(u)->append(t); }, &s};
}

// "{bob attack}  (boss.dta:4)"
inline std::string to_string(const StackEntry& entry) {
    std::string out;
    write(entry, string_out(out));
    return out;
}

// menu/'bacon and eggs', as Path::write writes it.
inline std::string to_string(const Path& path, size_t count = SIZE_MAX, char separator = '/') {
    std::string out;
    path.write(string_out(out), count, separator);
    return out;
}

// The compact single-line form of a node or an array: (price 12.75).
inline std::string to_string(const Node& node) {
    std::string out;
    write(node, string_out(out));
    return out;
}
inline std::string to_string(const Array& array) {
    std::string out;
    write(array, string_out(out));
    return out;
}

// The file form of a root array (write_dta).
inline std::string to_dta(const Array& root, const WriteOptions& options = {}) {
    std::string out;
    write_dta(root, string_out(out), options);
    return out;
}

// --- Convert<T> for standard types. ---

template <>
struct Convert<std::string> {
    static constexpr std::string_view name = "string";
    static constexpr bool scalar = true;
    static bool from(const Values& v, std::string& out) {
        std::string_view s;
        if (!Convert<std::string_view>::from(v, s)) return false;
        out.assign(s);
        return true;
    }
    static void to(const std::string& v, ValueList& out) { out.push(Node(String::make(v))); }
};

// Every value, each read as T: (tags fast loud blue) as std::vector<Symbol>.
template <class T>
struct Convert<std::vector<T>> {
    static constexpr std::string_view name = "list";
    static bool from(const Values& v, std::vector<T>& out) {
        out.assign(v.size(), T{});
        for (size_t i = 0; i < v.size(); ++i)
            if (!v.read(i, out[i])) return false;
        return true;
    }
    static void to(const std::vector<T>& v, ValueList& out) {
        for (const T& x : v) out.add(x);
    }
};

namespace detail {
// N of T's value type, for a T of one value of Baker's type; none for any other T, whose
// values nest in each element and cannot be listed flat.
template <class T, size_t N>
[[nodiscard]] constexpr auto repeated_value_types() noexcept {
    if constexpr (convert_info<T>().kind != ConvertKind::Compound && value_types<T>().size() == 1) {
        std::array<std::string_view, N> types{};
        for (std::string_view& t : types) t = value_types<T>()[0];
        return types;
    } else {
        return std::array<std::string_view, 0>{};
    }
}
}  // namespace detail

// Exactly N values, each read as T: (color 1 0.5 0) as std::array<float, 3>.
template <class T, size_t N>
struct Convert<std::array<T, N>> {
    static constexpr std::string_view name = "fixed-size list";
    // {"float32", "float32", "float32"} for std::array<float, 3>: the reference writes them,
    // and a tool checks without the game by them.
    static constexpr auto values = detail::repeated_value_types<T, N>();
    static bool from(const Values& v, std::array<T, N>& out) {
        if (v.size() != N) return false;
        for (size_t i = 0; i < N; ++i)
            if (!v.read(i, out[i])) return false;
        return true;
    }
    static void to(const std::array<T, N>& v, ValueList& out) {
        for (const T& x : v) out.add(x);
    }
};

// The names in a list of symbols, as strings (Config::layers, Config::unsaved).
inline std::vector<std::string> strings(const List<Symbol>& symbols) {
    std::vector<std::string> out;
    for (Symbol s : symbols) out.emplace_back(s.str());
    return out;
}

}  // namespace baker
