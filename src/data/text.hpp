#pragma once

#include "common/containers.hpp"

// Lexical rules shared by the parser and the printer. Keeping them in one place is what
// makes printing round-trip: the printer quotes exactly what the lexer would misread.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace baker {
class Array;
class Node;
class Path;
struct ListElement;
}  // namespace baker

namespace baker::detail {

// At most about `max_chars` of the compact form of `node`, ending in "..." if cut. Safe on
// huge or heavily shared trees; for error messages (write.cpp).
[[nodiscard]] Str preview(const Node& node, size_t max_chars = 40);
// 'name', as messages name a symbol.
[[nodiscard]] Str quoted(Symbol s);

// The text forms std_interop.hpp gives a host, for messages and edits inside Baker.
[[nodiscard]] Str text_of(const Path& path, size_t count = SIZE_MAX, char separator = '/');
[[nodiscard]] Str text_of(const Node& node);
[[nodiscard]] Str dta_of(const Array& root);

// JSON, for the machine-readable outputs (check, the reference, replies): one string,
// escaped (quote, backslash, control characters; UTF-8 as it is), and a list of names.
void json_string(Str& out, std::string_view s);
void json_names(Str& out, std::span<const Symbol> names);
// `, "element": ..., "length": ...` for a tweak or a parameter: the list's element as
// {"type", "values", "count"} and its fixed length, each null where there is none.
void json_list_element(Str& out, const ListElement& element);

// The bytes that end an atom: control characters and space, the six brackets, a quote and
// a comment. A table, so the lexer's scan is one load a byte.
inline constexpr std::array<bool, 256> kAtomEnd = [] {
    std::array<bool, 256> t{};
    for (int c = 0; c <= 0x20; ++c) t[static_cast<size_t>(c)] = true;
    for (const char c : std::string_view("(){}[]\";")) t[static_cast<unsigned char>(c)] = true;
    return t;
}();
[[nodiscard]] constexpr bool is_atom_end(unsigned char c) noexcept {
    return kAtomEnd[c];
}

enum class NumberKind : uint8_t { NotNumber, Int, Float, IntOutOfRange, FloatOutOfRange };

struct Number {
    NumberKind kind = NumberKind::NotNumber;
    int64_t i = 0;
    double f = 0;
};

// Classifies a bare atom: decimal or 0x-hex integer (optionally signed), decimal float,
// or not a number. Integers must fit int64; a hex literal denotes its unsigned value, so
// it must be at most 0x7FFFFFFFFFFFFFFF. Floats must be finite doubles.
[[nodiscard]] Number classify_number(std::string_view atom) noexcept;

// True if `s` reads back as this symbol when written bare (unquoted).
[[nodiscard]] bool is_bare_symbol(std::string_view s) noexcept;

// Appends a path segment naming `name`: bare where a Path reads it back as that name,
// otherwise 'quoted'.
void append_path_name(Str& out, std::string_view name);

// Shortest text that reads back as exactly `v` (by classify_number), such as "12.75",
// "5", "1e+20" or "-0". NaN and infinities print as nan/inf/-inf. The caller adds ".0"
// where needed to keep integral values floats.
[[nodiscard]] Str format_double(double v);
// The portable implementation used where std::to_chars for doubles is unavailable (older
// Apple deployment targets); exposed for tests.
[[nodiscard]] Str format_double_fallback(double v);

// Lexical path handling. Baker never touches the file system itself (LoadSession's reader
// does), so include paths are only joined and normalized as text. Both '/' and '\\' are
// separators; results use '/'. A path that begins with a scheme, "user://a.dta" (an engine's
// virtual root), is absolute, and its "name://" is kept as written.
[[nodiscard]] bool is_absolute_path(std::string_view p) noexcept;
// Removes "." and empty segments and resolves ".." lexically: "a/./b/../c.dta" -> "a/c.dta".
[[nodiscard]] Str normalize_path(std::string_view p);
// Everything before the last separator: "a/b/c.dta" -> "a/b"; "c.dta" -> ""; a root is its
// own parent: "/a" -> "/", "C:/a" -> "C:/", "user://a" -> "user://".
[[nodiscard]] std::string_view parent_path(std::string_view p) noexcept;
[[nodiscard]] Str join_path(std::string_view dir, std::string_view p);

// `path`, terminated for fopen: on the stack unless it is long, so opening a file need not
// allocate (not even MSVC's debug proxy for an empty string).
class CPath {
public:
    explicit CPath(std::string_view path);
    [[nodiscard]] const char* c_str() const noexcept { return p_; }

private:
    char buf_[512];
    std::optional<Str> long_;
    const char* p_ = buf_;
};

}  // namespace baker::detail
