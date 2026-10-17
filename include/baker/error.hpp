#pragma once

// Error: the structured description of everything that can go wrong in Baker.
//
// Errors carry data, not prose: what kind of failure, a message, and where it happened
// (file, line, and the array path that led there). Formatting is the caller's business;
// to_string() gives the canonical one-line form:
//
//   baker: 'price' is float, not int  (menu.dta:3, at menu/'bacon and eggs')
//
// An error from a script also carries the script call stack it unwound through
// (Error::stack); describe() renders it, one frame a line, innermost first.
//
// Baker never throws: every fallible API returns Expected<T>, and the
// library is built with exceptions disabled.

#include <baker/expected.hpp>
#include <baker/symbol.hpp>

#include <baker/text.hpp>

#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace baker {

// BAKER_SHIPPING: reload and live edits are inert, and dev_only tweaks keep their code
// defaults (config.hpp, tweak.hpp).
#if defined(BAKER_SHIPPING) && BAKER_SHIPPING
inline constexpr bool kShipping = true;
#else
inline constexpr bool kShipping = false;
#endif

enum class ErrorCode : uint8_t {
    Syntax,     // malformed text
    Range,      // a value out of its representable range
    Directive,  // a preprocessor directive used incorrectly
    Io,         // a file could not be read or written
    Limit,      // a safety limit (nesting, node count, includes) was exceeded
    Type,       // a value has the wrong type for how it was accessed
    NotFound,   // a lookup found nothing
    Refused,    // an operation was declined: write-back without one literal source, a
                // mutation of a frozen config
};

[[nodiscard]] std::string_view error_code_name(ErrorCode code) noexcept;

// One call on the script call stack: {on function ...} written at file:line. `on` is the
// object a method was sent to, when it has a name; empty for a function.
struct StackEntry {
    Symbol function;
    Symbol on;
    Symbol file;
    int32_t line = 0;
};

namespace detail {
struct ErrorRecord;
}

// An error is a handle to a record allocated on the error path only, so Expected<T> costs
// what T costs plus a pointer. It copies like a value: a copy is
// a separate record. A default-constructed Error is empty and means "no error".
class Error {
public:
    Error() noexcept = default;
    Error(ErrorCode code, std::string_view message, Symbol file = {}, int32_t line = 0);
    Error(const Error& other);
    Error(Error&& other) noexcept : r_(other.r_) { other.r_ = nullptr; }
    Error& operator=(const Error& other);
    Error& operator=(Error&& other) noexcept;
    ~Error();

    [[nodiscard]] bool empty() const noexcept { return r_ == nullptr; }
    [[nodiscard]] ErrorCode code() const noexcept;
    [[nodiscard]] std::string_view message() const noexcept;
    [[nodiscard]] Symbol file() const noexcept;   // empty when unknown
    [[nodiscard]] int32_t line() const noexcept;  // 0 when unknown
    // 1-based, in characters (code points) from the line's start; 0 when unknown. The parser
    // gives one; a lookup or shape error, located by an array's line, does not.
    [[nodiscard]] int32_t column() const noexcept;
    // The array path, e.g. menu/'bacon and eggs'; empty when unknown.
    [[nodiscard]] std::string_view path() const noexcept;
    // The script calls it unwound through, innermost first.
    [[nodiscard]] std::span<const StackEntry> stack() const noexcept;

    // Changing an error, as it is located on the way out.
    void set_message(std::string_view message);
    // Sets the file and line, and clears the column.
    void set_location(Symbol file, int32_t line);
    void set_column(int32_t column);
    void set_path(std::string_view path);
    void push_stack(const StackEntry& entry);

private:
    friend std::string_view to_string(const Error& error);
    friend std::string_view describe(const Error& error);
    detail::ErrorRecord* r_ = nullptr;
};

[[nodiscard]] Error make_error(ErrorCode code, std::string_view message, Symbol file = {},
                               int32_t line = 0);

// "baker: <message>  (<file>:<line>:<column>, at <path>)", omitting whatever is unknown. The text
// is kept in the error and null-terminated; the view is valid while the error lives and is not
// changed.
[[nodiscard]] std::string_view to_string(const Error& error);
// to_string, then a line per stack entry, innermost first:
//   baker: '%' by zero  (boss.dta:9)
//     in {%}  (boss.dta:9)
//     in {bob attack}  (boss.dta:4)
[[nodiscard]] std::string_view describe(const Error& error);
// Writes "{bob attack}  (boss.dta:4)".
void write(const StackEntry& entry, TextSink out);

template <class T>
using Expected = expected<T, Error>;
using Unexpected = unexpected<Error>;

#if !BAKER_HAS_STD_EXPECTED
namespace detail {
// Reading the value of an Expected that holds an error is a bug in the caller: abort.
template <class E>
[[noreturn]] void throw_bad_expected_access(const E&) {
    std::abort();
}
}  // namespace detail
#endif

}  // namespace baker
