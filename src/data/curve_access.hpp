#pragma once

// Curve's friend (curve.hpp): sets what a curve reads its points from, for the tree reader
// (curve.cpp) and for a baked table's relocation (table.cpp).

#include <baker/curve.hpp>

namespace baker {

struct CurveReader {
    static void set(Curve& c, const Node* nodes, uint32_t size, CurveMode mode,
                    CurveExtend extend) noexcept {
        c = Curve();
        c.nodes_ = nodes;
        c.size_ = size;
        c.mode_ = mode;
        c.extend_ = extend;
    }
    [[nodiscard]] static const CurvePoint* points(const Curve& c) noexcept { return c.points_; }
    static void set_points(Curve& c, const CurvePoint* p) noexcept {
        c.points_ = p;
        c.nodes_ = nullptr;
    }
};

}  // namespace baker
