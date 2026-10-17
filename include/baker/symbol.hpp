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

namespace detail {

struct SymbolEntry {
    const char* text;  // NUL-terminated
    uint32_t size;
    uint32_t index;  // dense, in interning order; 0 is the empty symbol
    uint64_t hash;
};

[[nodiscard]] constexpr uint64_t hash_symbol_text(std::string_view s) noexcept {
    uint64_t h = 14695981039346656037ull;  // FNV-1a
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h;
}

inline constexpr SymbolEntry kEmptySymbolEntry{"", 0, 0, hash_symbol_text("")};

}  // namespace detail

class Symbol {
public:
    // The empty symbol.
    constexpr Symbol() noexcept : e_(&detail::kEmptySymbolEntry) {}
    // Interns `text`. Implicit so that names can be written as string literals.
    Symbol(std::string_view text);  // NOLINT(google-explicit-constructor)
    Symbol(const char* text) : Symbol(std::string_view(text)) {}  // NOLINT

    [[nodiscard]] std::string_view str() const noexcept { return {e_->text, e_->size}; }
    [[nodiscard]] const char* c_str() const noexcept { return e_->text; }
    [[nodiscard]] uint64_t hash() const noexcept { return e_->hash; }
    [[nodiscard]] bool empty() const noexcept { return e_->size == 0; }
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

inline namespace literals {
// "price"_sym
inline Symbol operator""_sym(const char* s, size_t n) {
    return Symbol(std::string_view(s, n));
}
}  // namespace literals

}  // namespace baker
