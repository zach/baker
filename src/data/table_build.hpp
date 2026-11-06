#pragma once

// A Config's baked tables (table.hpp): the registered specs and their blocks, and the
// builder that makes a block from the merged tree.

#include <baker/table.hpp>

#include "common/containers.hpp"

namespace baker::detail {

struct TableSlot {
    explicit TableSlot(const Allocator& a) : at(a), sources(a), errors(a) {}
    TableSlot(TableSlot&&) noexcept = default;
    TableSlot& operator=(TableSlot&&) noexcept = default;
    TableSpec spec;
    Vec<Symbol> at;  // the shape's (@at ...) segments; `*` is any tag
    Ref<const BakedTable> built;
    // The arrays whose children are the records, in tree order, as of the last build: a
    // tree that shares them all has the same records.
    Vec<const Array*> sources;
    Vec<Error> errors;  // the last build's, as given to the warning handler
};

// Reads the spec's `at` into the slot; refused when it has no segment (a keyed shape is
// at a place).
[[nodiscard]] Expected<void> parse_table_at(const TableSpec& spec, TableSlot& slot);
// Whether `root` holds the same governing arrays the slot was built from.
[[nodiscard]] bool table_sources_unchanged(const TableSlot& slot, const Array& root,
                                           const Allocator& alloc);
// Builds the slot's table from `root`: one block from `alloc`; errors in records (a field
// that is not a literal, a wrong type) to `warn`, located, the member left at its default.
// Records the sources in the slot.
[[nodiscard]] Ref<const BakedTable> build_table(TableSlot& slot, const Array& root,
                                                const Allocator& alloc,
                                                const Callback<void(const Error&)>& warn);
// Records in the slot the governing arrays `root` holds, without building: for a table
// taken from an image, so the next change compares against them.
void record_table_sources(TableSlot& slot, const Array& root, const Allocator& alloc);

// The image's table section (image.hpp): the block's bytes with every pointer as an
// offset and every symbol by name, and what the reader checks against its own view.
[[nodiscard]] Expected<void> write_table(const BakedTable& table, const TableSpec& spec, Str& out);
// A block from a section, or null with `why` set: the section is not for `spec` (another
// digest, row size or fix-up list) or is damaged.
[[nodiscard]] Ref<const BakedTable> read_table(std::string_view bytes, const TableSpec& spec,
                                               const Allocator& alloc, Error& why);
// A Config's table slots, for the image writer.
[[nodiscard]] std::span<const TableSlot> table_slots(const Config& cfg);

}  // namespace baker::detail
