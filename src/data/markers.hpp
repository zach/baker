#pragma once

// The layer verbs: `(key #remove)`, `(key #append v...)` and `(key #drop v...)`, and
// the `(@was v...)` pin. A verb is a reserved symbol in a tagged array's first value slot,
// which a Config's merge consumes, so no merged tree holds one; the pin is metadata, which
// the merged tree keeps. The parser, the printer, the merge and the layer tools all read the
// forms from here.

#include <baker/array.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace baker::detail {

enum class Marker : uint8_t { None, Remove, Append, Drop };

[[nodiscard]] inline const Symbol& marker_symbol(Marker m) {
    static const Symbol symbols[] = {Symbol(), Symbol("#remove"), Symbol("#append"),
                                     Symbol("#drop")};
    return symbols[static_cast<size_t>(m)];
}

// The verb a symbol is, or None: three identity compares.
[[nodiscard]] inline Marker marker_of(Symbol s) noexcept {
    for (uint8_t m = 1; m <= 3; ++m)
        if (s == marker_symbol(static_cast<Marker>(m))) return static_cast<Marker>(m);
    return Marker::None;
}
[[nodiscard]] inline Marker marker_of(const Node& n) noexcept {
    const auto s = n.try_sym();
    return s ? marker_of(*s) : Marker::None;
}

// The verb the directive `#name` names, or None.
[[nodiscard]] inline Marker marker_named(std::string_view name) noexcept {
    return name == "remove"   ? Marker::Remove
           : name == "append" ? Marker::Append
           : name == "drop"   ? Marker::Drop
                              : Marker::None;
}

// The metadata tag of a row's pin: the values below the layer when the row was written.
[[nodiscard]] inline const Symbol& was_tag() {
    static const Symbol s("@was");
    return s;
}
// A record's identity, (@id ...), and the keys it still answers to, (@aka ...).
// The symbol an array's (@id ...) holds, as written, or empty: what a layer's child tagged
// with the ID matches. Array::record_id() parses it; this compares it.
[[nodiscard]] Symbol id_symbol(const Array& a) noexcept;
// Whether any array below `root` (or `root`) carries an (@aka ...) or an (@id ...): what a
// parse counts, for a tree that was not parsed here. A walk, out at the first.
[[nodiscard]] bool has_identity(const Array& root) noexcept;
[[nodiscard]] inline const Symbol& id_tag() {
    static const Symbol s("@id");
    return s;
}
[[nodiscard]] inline const Symbol& aka_tag() {
    static const Symbol s("@aka");
    return s;
}

// The verb of a layer row, in the form the parser takes: `#remove` as the one value, or
// `#append`/`#drop` first with one or more operands, metadata after either. None for any
// other array, after one flag test for an array that holds no verb or pin. The verb is the
// symbol in its slot, however it was spelled: a quoted '#remove' there is one.
// The verb of an array whose flag is set (merge.cpp).
[[nodiscard]] Marker verb_in(const Array& a) noexcept;
// A hash that agrees with structurally_equal: equal nodes hash the same (merge.cpp).
[[nodiscard]] uint64_t structural_hash(const Node& n, uint64_t h = 1) noexcept;
[[nodiscard]] inline Marker verb_of(const Array& a) noexcept {
    return a.has_marker() ? verb_in(a) : Marker::None;
}
[[nodiscard]] inline bool is_removal(const Array& a) noexcept {
    return verb_of(a) == Marker::Remove;
}
// Whether `a` changes the value below it without supplying one: (key #append v...) or
// (key #drop v...). Such a row is never a value's source.
[[nodiscard]] inline bool composes(const Array& a) noexcept {
    const Marker m = verb_of(a);
    return m == Marker::Append || m == Marker::Drop;
}

// The form each verb is written in: the hint of its Directive error.
[[nodiscard]] inline const char* marker_hint(Marker m) noexcept {
    static const char* const hints[] = {
        "",
        "#remove is written as a key's only value: (key #remove)",
        "#append is written as a key's first value, with what it adds: (key #append v...)",
        "#drop is written as a key's first value, with what it removes: (key #drop v...)",
    };
    return hints[static_cast<size_t>(m)];
}

}  // namespace baker::detail
