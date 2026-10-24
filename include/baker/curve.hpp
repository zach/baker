#pragma once

// A curve as a value: a function designers tune, such as reward by stage, difficulty by time,
// or falloff by distance. A curve is a key's values, points (x y) in ascending x:
//
//   (falloff (0 1) (5 3) (10 8))                    ; monotone, clamped
//   (difficulty catmull-rom loop (0 1) (30 4) (60 1))
//   (reward linear (1 10) (2 25 0 15) (3 40))       ; (x y in out): the slopes at that point
//
// Before the points, two optional symbols, in either order:
//   - the interpolation, for the whole curve: `monotone` (the default; smooth, and never
//     above or below the two keys around it), `linear`, `catmull-rom` (smooth; may overshoot
//     between keys that swing), or `constant` (each key's value held to the next key). The
//     names are RenderMan's and Houdini's.
//   - what lies beyond the points: `clamp` (the default; the end keys' values),
//     `extrapolate` (on along the end keys' slopes), or `loop` (repeating the span of x).
// A point may give its in and out slopes, dy/dx as glTF's Hermite tangents are, as a third
// and fourth number: they replace the slopes its interpolation would compute there, on
// either side (in: the segment before, out: the one after). A `constant` curve has no
// slopes. Godot stores a point's tangents as angles: atan of these.
//
//   const baker::Curve c = cfg.get<baker::Curve>("falloff").value();
//   float strength = static_cast<float>(c.sample(distance));
//
// A Curve read from a tree views its nodes: it is valid while that tree lives, as a
// std::string_view read from one is. Tweak<Curve> keeps its tree, so its curve stays valid
// until the next reload. A layer replaces a whole curve, as it replaces any key's values. A
// shape's field type `curve` checks one without the game.

#include <baker/array.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace baker {

enum class CurveMode : uint8_t { Monotone, Linear, CatmullRom, Constant };
enum class CurveExtend : uint8_t { Clamp, Extrapolate, Loop };

// One point of a curve: x, y, and the slopes at it if it gives them.
struct CurvePoint {
    double x = 0, y = 0;
    double in = 0, out = 0;  // dy/dx on each side; read only if `sloped`
    bool sloped = false;
};

class Curve {
public:
    // No points: every sample is 0.
    constexpr Curve() noexcept = default;
    // Over points the caller keeps, in ascending x: a curve a host builds, to set a tweak.
    constexpr Curve(std::span<const CurvePoint> points, CurveMode mode = CurveMode::Monotone,
                    CurveExtend extend = CurveExtend::Clamp) noexcept
        : points_(points.data()),
          size_(static_cast<uint32_t>(points.size())),
          mode_(mode),
          extend_(extend) {}

    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] CurvePoint point(size_t i) const noexcept;
    [[nodiscard]] CurveMode mode() const noexcept { return mode_; }
    [[nodiscard]] CurveExtend extend() const noexcept { return extend_; }

    // The curve's value at `x`: a binary search for the segment, then its arithmetic.
    // Allocates nothing.
    [[nodiscard]] double sample(double x) const noexcept;

private:
    friend struct CurveReader;
    const Node* nodes_ = nullptr;  // read from a tree: each point an array (x y [in out])
    const CurvePoint* points_ = nullptr;
    uint32_t size_ = 0;
    CurveMode mode_ = CurveMode::Monotone;
    CurveExtend extend_ = CurveExtend::Clamp;

    [[nodiscard]] double x_at(size_t i) const noexcept;  // the search reads x alone
    [[nodiscard]] double slope_in(size_t i) const noexcept;
    [[nodiscard]] double slope_out(size_t i) const noexcept;
    [[nodiscard]] double computed_slope(size_t i) const noexcept;
    [[nodiscard]] double end_slope(size_t i) const noexcept;
    [[nodiscard]] double within(double x) const noexcept;
};

// Why values do not form a curve, and at which of them; `what` is null when they do.
struct CurveProblem {
    const char* what = nullptr;
    size_t at = 0;
    explicit operator bool() const noexcept { return what != nullptr; }
};

// Reads `values` as a curve into `out`, or says why they are not one: a mode or extension
// that is not one of the names, a point that is not (x y) or (x y in out) in numbers, x not
// ascending, or no points. Allocates nothing; `out` views `values`' nodes.
[[nodiscard]] CurveProblem read_curve(const Values& values, Curve& out) noexcept;

template <>
struct Convert<Curve> {
    static constexpr std::string_view name = "curve";
    static bool from(const Values& v, Curve& out) noexcept { return !read_curve(v, out); }
    // The mode and extension when they are not the defaults, then each point.
    static void to(const Curve& c, ValueList& out);
};

}  // namespace baker
