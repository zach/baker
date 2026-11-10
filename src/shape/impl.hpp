#pragma once

// What shape.cpp shares with shape_tools.cpp: the check's walk, and the spellings both
// write. Internal to baker::shape.

#include <baker/shape.hpp>

#include "common/containers.hpp"

#include <string_view>

namespace baker::shape::impl {

// Checks `tree` against `shapes` into `errors` and `warnings`, and gathers what each
// non-null argument asks for: each shape that governed a record (`used`, by index), each
// alias read, each renamed field, each key and asset value.
void run(const Array& tree, const Set& shapes, Vec<Error>& errors, Vec<Error>& warnings,
         const LoadSession* packs, Vec<bool>* used, Vec<baker::Symbol>* aliases_used = nullptr,
         Vec<Renamed>* renames = nullptr, Vec<KeyUse>* key_uses = nullptr,
         Vec<AssetUse>* asset_uses = nullptr);

// A type's name as a shape writes it.
[[nodiscard]] std::string_view type_text(Type t);
// A key field's table as written, dotted; empty for any other field.
[[nodiscard]] Str table_path(const Field& f);

}  // namespace baker::shape::impl
