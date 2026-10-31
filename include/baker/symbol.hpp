#pragma once

// Symbol: an interned, immutable string compared by pointer.
//
// Symbols live in a process-global, append-only table. Interning is O(1) hashed and
// thread-safe; comparison is a pointer comparison; each symbol carries its precomputed
// hash. Symbol memory is never reclaimed. That is the original design and it is correct
// here: the set of distinct names in a game's data and code is small and stable, and
// immortality is what makes a Symbol safe to copy into any table, cache, or static.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace baker {

// A name's stable 64-bit ID: the hash a Symbol of the same text carries (Symbol::hash()),
// computed at compile time when the text is a constant. For IDs that must outlive the
// process, such as a switch's case labels, a save file, a network message, a replay or a
// crash dump, where a Symbol's pointer or interning order means nothing:
//
//   switch (event.hash()) {
//       case "jump"_hash: ...; break;
//       case "land"_hash: ...; break;
//   }
//
// The value is FNV-1a 64 over the text's bytes, as written (UTF-8 for a non-ASCII name),
// with no normalisation: the same on every platform, compiler and build mode. It is a
// promise: a change of the algorithm is a breaking change, and the library's own build
// checks golden values. A hash is an extra key for a game, never the name's replacement:
// data files and caches keep the text. Two names may share a hash, if very rarely; a build
// that is not shipping reports it (set_symbol_collision_handler, <baker/error.hpp>).
[[nodiscard]] constexpr uint64_t symbol_hash(std::string_view text) noexcept {
    uint64_t h = 14695981039346656037ull;  // FNV-1a 64: offset basis, then prime
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h;
}

namespace detail {

struct SymbolEntry {
    const char* text;  // NUL-terminated
    uint32_t size;     // the high bit: kSymbolMetaBit, set when interned
    uint32_t index;    // dense, in interning order; 0 is the empty symbol
    uint64_t hash;
};

// In SymbolEntry::size: the text is '@' and at least one more character, the metadata
// sigil. A lone '@' is a symbol like any other.
inline constexpr uint32_t kSymbolMetaBit = 0x80000000u;
inline constexpr uint32_t kSymbolSizeMask = ~kSymbolMetaBit;

inline constexpr SymbolEntry kEmptySymbolEntry{"", 0, 0, symbol_hash("")};

// Interns `text` whose symbol_hash is `hash`, as "x"_sym does with the hash its compile
// computed. The hash must be symbol_hash(text).
[[nodiscard]] const SymbolEntry* intern_hashed(std::string_view text, uint64_t hash);

}  // namespace detail

class Symbol {
public:
    // The empty symbol.
    constexpr Symbol() noexcept : e_(&detail::kEmptySymbolEntry) {}
    // Interns `text`. Implicit so that names can be written as string literals.
    Symbol(std::string_view text);  // NOLINT(google-explicit-constructor)
    Symbol(const char* text) : Symbol(std::string_view(text)) {}  // NOLINT

    [[nodiscard]] std::string_view str() const noexcept {
        return {e_->text, e_->size & detail::kSymbolSizeMask};
    }
    [[nodiscard]] const char* c_str() const noexcept { return e_->text; }
    [[nodiscard]] uint64_t hash() const noexcept { return e_->hash; }
    [[nodiscard]] bool empty() const noexcept { return e_->size == 0; }
    // Whether the text is '@' and more, (@range 0 10)'s tag: an array tagged so is metadata.
    // One load of the size word, its bit set when the symbol was interned.
    [[nodiscard]] bool is_meta() const noexcept { return (e_->size & detail::kSymbolMetaBit) != 0; }
    // Dense index, in interning order (the empty symbol is 0), for tables indexed by
    // symbol: baker::script keeps its functions and variables this way.
    [[nodiscard]] uint32_t index() const noexcept { return e_->index; }

    friend bool operator==(Symbol a, Symbol b) noexcept { return a.e_ == b.e_; }

    // Raw identity, for storage inside Node and binary formats. Not for general use.
    [[nodiscard]] const void* id() const noexcept { return e_; }
    [[nodiscard]] static Symbol from_id(const void* id) noexcept {
        return Symbol(static_cast<const detail::SymbolEntry*>(id));
    }

    // Number of distinct symbols interned so far (diagnostics and tests).
    [[nodiscard]] static size_t table_size() noexcept;

private:
    explicit constexpr Symbol(const detail::SymbolEntry* e) noexcept : e_(e) {}
    const detail::SymbolEntry* e_;
};

namespace detail {
// A string literal as a template argument, for the literal operator below.
template <size_t N>
struct SymbolLiteral {
    constexpr SymbolLiteral(const char (&s)[N]) noexcept {  // NOLINT(google-explicit-constructor)
        for (size_t i = 0; i < N; ++i) text[i] = s[i];
    }
    [[nodiscard]] constexpr std::string_view view() const noexcept { return {text, N - 1}; }
    char text[N]{};
};
}  // namespace detail

inline namespace literals {
// "price"_sym: interned once per distinct literal, on its first call, then one load. A literal in a
// hot loop costs what a Symbol held in a variable costs; Symbol("price") interns each call. Its
// hash is the compile's, so the first call does not hash.
template <detail::SymbolLiteral S>
Symbol operator""_sym() {
    static constexpr uint64_t hash = symbol_hash(S.view());
    static const Symbol symbol = Symbol::from_id(detail::intern_hashed(S.view(), hash));
    return symbol;
}

// "jump"_hash: symbol_hash("jump"), always at compile time, for a case label.
consteval uint64_t operator""_hash(const char* text, size_t size) {
    return symbol_hash({text, size});
}
}  // namespace literals

}  // namespace baker
