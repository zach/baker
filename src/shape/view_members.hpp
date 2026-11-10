#pragma once

// The order of a view's members: what the generated struct declares, and what its
// digest spells, so a struct laid out another way has another digest.

#include <baker/shape.hpp>

#include "common/containers.hpp"

namespace baker::shape {

// The fields of `s` its view holds, in the struct's order: by alignment, largest first (a
// pointer counts as 8 bytes), and in declaration order within one alignment, so the
// struct carries no padding between members that this order can avoid.
[[nodiscard]] Vec<const Field*> view_members(const Set& set, const Shape& s);

}  // namespace baker::shape
