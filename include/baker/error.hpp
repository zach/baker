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

// Marks a function rarely called, as std::abort is, so the code calling it is laid out as if
// it did not.
#if defined(__GNUC__)
#define BAKER_DETAIL_COLD [[gnu::cold]]
#else
#define BAKER_DETAIL_COLD
#endif

namespace baker {

// BAKER_SHIPPING: reload and live edits are inert, and dev_only tweaks keep their code
// defaults (config.hpp, tweak.hpp).
#if defined(BAKER_SHIPPING) && BAKER_SHIPPING
inline constexpr bool kShipping = true;
#else
inline constexpr bool kShipping = false;
#endif

// The game and Baker must agree on BAKER_SHIPPING and BAKER_ATOMIC_REFCOUNT: both change the
// layout of types the two share. CMake's targets pass Baker's choice on; a game built
// another way defines them as Baker was built. A mismatch fails at link time: MSVC
// names both values, and elsewhere a tweak's registration names the configuration the
// game was compiled with, which a library built the other way lacks.
#if defined(_MSC_VER) && defined(BAKER_SHIPPING) && BAKER_SHIPPING
#pragma detect_mismatch("BAKER_SHIPPING", "1")
#elif defined(_MSC_VER)
#pragma detect_mismatch("BAKER_SHIPPING", "0")
#endif
namespace detail {
struct ConfiguredForShipping {};
struct ConfiguredForDevelopment {};
using Configuration =
    std::conditional_t<kShipping, ConfiguredForShipping, ConfiguredForDevelopment>;
}  // namespace detail

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
    Collision,  // two names share a hash (set_symbol_collision_handler)
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

// A fatal error: Baker cannot go on. An allocator returned null, an array or a string passed
// 4 GB, the value of an Expected that holds an error was read, a symbol collision went to the
// default handler. The handler gets a message, and after it the current Context's script call
// stack, innermost first, when a script call is running: for an engine's crash reporter. The
// stack is that of this thread's current Context (Context::current: the last made on the
// thread, or one a Context::Scope sets); a Context whose calls run on a job thread is made
// current there with a Scope, or the stack is another thread's. The handler must not
// return; if it does, Baker aborts. The message is built in a buffer on the stack,
// since what failed may be the allocator, and is cut at 4 KB. Returns the handler it replaces;
// null restores the default, which writes the message to stderr.
using FatalHandler = void (*)(std::string_view message);
FatalHandler set_fatal_handler(FatalHandler handler) noexcept;

namespace detail {
// Reports a fatal error, `what` then `more`, to the fatal handler, then aborts. Cold, as
// std::abort is, so a check that calls it compiles as one that aborts did.
[[noreturn]] BAKER_DETAIL_COLD void fatal(std::string_view what,
                                          std::string_view more = {}) noexcept;
// Writes the current Context's script call stack, if a call is running: baker::script sets it
// when it makes its first Context, so a fatal error carries the stack.
using FatalStack = void (*)(TextSink out);
void set_fatal_stack(FatalStack stack) noexcept;
}  // namespace detail

// Two names that share a hash (symbol_hash). Symbols stay correct, since interning compares
// text, but code that keys on Symbol::hash(), a switch over "x"_hash or a save keyed by hash,
// cannot tell the two apart. A build that is not shipping checks each name the first time it
// is interned, and reports such a pair to this handler: an Error (ErrorCode::Collision)
// naming both names and their hash, located at the data file and line being read when the
// name came from one. Both names stay interned, distinct Symbols. The default handler writes
// the report to stderr and aborts. A handler may intern names; it runs outside the symbol
// table's lock, on the thread that interned the second name. Returns the handler it
// replaces; null restores the default. Shipping builds do not check, and never call it.
using SymbolCollisionHandler = void (*)(const Error& report);
SymbolCollisionHandler set_symbol_collision_handler(SymbolCollisionHandler handler) noexcept;
// Writes "{bob attack}  (boss.dta:4)".
void write(const StackEntry& entry, TextSink out);

template <class T>
using Expected = expected<T, Error>;
using Unexpected = unexpected<Error>;

#if !BAKER_HAS_STD_EXPECTED
namespace detail {
// Reading the value of an Expected that holds an error is a bug in the caller: abort.
template <class E>
[[noreturn]] void throw_bad_expected_access(const E& e) {
    if constexpr (std::is_same_v<E, Error>)
        fatal("the value of an Expected that holds an error was read: ", to_string(e));
    else
        fatal("the value of an Expected that holds an error was read");
}
}  // namespace detail
#endif

}  // namespace baker

#undef BAKER_DETAIL_COLD  // used above only
