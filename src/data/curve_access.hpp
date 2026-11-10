#pragma once

// Curve's friend (curve.hpp): sets what a curve reads its points from, for the tree reader
// (curve.cpp) and for a baked table's relocation (table.cpp).

#include <baker/curve.hpp>

namespace baker {

struct CurveReader {
    // `source`: the array whose values `nodes` are, or null when they are no array's.
    static void set(Curve& c, const Node* nodes, uint32_t size, CurveMode mode, CurveExtend extend,
                    const Array* source) noexcept {
        c = Curve();
        c.nodes_ = nodes;
        if (source) {
            c.source_ = source;
            c.sourced_ = true;
        }
        c.size_ = size;
        c.mode_ = mode;
        c.extend_ = extend;
    }
    [[nodiscard]] static const CurvePoint* points(const Curve& c) noexcept {
        return c.nodes_ ? nullptr : c.points_;
    }
    static void set_points(Curve& c, const CurvePoint* p) noexcept {
        c.points_ = p;
        c.nodes_ = nullptr;
        c.sourced_ = false;
    }
};

namespace detail {
// The baked copy of the curve whose points are `nodes`, `size` of them, values of `source`,
// kept by source's load: found, or with `bake`, made on a miss (the nodes must be a curve's,
// as read_curve checks). Null if source is in no load, or the load's cache is full, or
// without `bake`, when there is no copy yet. Lock-free when found; safe on any thread.
[[nodiscard]] const CurvePoint* baked_curve(const Array& source, const Node* nodes, uint32_t size,
                                            bool bake) noexcept;
}  // namespace detail

}  // namespace baker
