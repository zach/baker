#pragma once

// Filling a solo array its caller alone holds, in place, without the checks of
// Array::set: Context::call's commands, which it reuses from call to call.

#include <baker/array.hpp>

#include <span>

namespace baker::detail {

struct ArrayFill {
    // Puts `first`, `second`, then `rest` into `a`, a solo array of exactly that many nodes,
    // each Int 0 (as Array::make or clear() leaves it), that nothing else holds. The height,
    // the tag run and the flags are then those Array::make_copy gives the same nodes.
    static void fill(Array& a, const Node& first, const Node& second,
                     std::span<const Node> rest) noexcept;
    // Every node back to Int 0, and the height, tag run and flags as Array::make leaves
    // them.
    static void clear(Array& a) noexcept;
};

}  // namespace baker::detail
