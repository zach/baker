#pragma once

// A record's identity (the schema layer's W3): 128 random bits written
// inside the record as metadata, so it travels with the data and survives a rename of the
// record's key:
//
//   (weapons (laser (@id b3c9a7e5f214d68a1e2c3b4d5e6f7081) (damage 3)))
//
// Spelled as 32 lowercase hex digits whose first is `a` to `f`, so the token is a symbol to
// the lexer and never a number. Tools mint IDs (the host kit's `format --baker-mint-ids`,
// `rename`); the library never does. References keep naming keys (`key_of`); the ID
// anchors renames, change records and tools: `Array::record_id()` reads one,
// `find_by_id` finds the record that carries one, and `id_validator()` reports two records
// sharing one (<baker/config.hpp>).

#include <cstdint>
#include <optional>
#include <string_view>

namespace baker {

struct RecordId {
    uint64_t hi = 0;
    uint64_t lo = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return hi == 0 && lo == 0; }
    friend constexpr bool operator==(RecordId a, RecordId b) noexcept {
        return a.hi == b.hi && a.lo == b.lo;
    }
    friend constexpr bool operator<(RecordId a, RecordId b) noexcept {
        return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
    }

    static constexpr size_t kDigits = 32;
    // The ID `text` spells, or nothing: 32 lowercase hex digits, the first `a` to `f`.
    [[nodiscard]] static std::optional<RecordId> parse(std::string_view text) noexcept;
    // Writes the ID as parse reads it. An ID whose first digit is not a letter cannot be
    // written as a symbol: write() gives it anyway, and valid() says whether it may be.
    void write(char (&out)[kDigits]) const noexcept;
    [[nodiscard]] constexpr bool valid() const noexcept { return (hi >> 60) >= 0xA; }
};

}  // namespace baker
