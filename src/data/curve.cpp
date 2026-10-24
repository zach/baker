// Curves: reading one from a key's values, and sampling it.

#include <baker/curve.hpp>

#include <cmath>

namespace baker {

namespace {

struct CurveNames {
    Symbol monotone{"monotone"}, linear{"linear"}, catmull_rom{"catmull-rom"}, constant{"constant"},
        clamp{"clamp"}, extrapolate{"extrapolate"}, loop{"loop"};
};
const CurveNames& curve_names() {
    static const CurveNames n;
    return n;
}

// A point written (x y) or (x y in out), in numbers.
bool point_of(const Node& n, CurvePoint& p) noexcept {
    const Array* a = n.try_array();
    if (!a || (a->size() != 2 && a->size() != 4)) return false;
    double v[4] = {};
    for (uint32_t i = 0; i < a->size(); ++i) {
        const auto x = (*a)[i].try_float();
        if (!x || !std::isfinite(*x)) return false;
        v[i] = *x;
    }
    p = {v[0], v[1], v[2], v[3], a->size() == 4};
    return true;
}

// The cubic Hermite segment from (x0 y0), slope m0, to (x1 y1), slope m1, at x.
double hermite(double x0, double y0, double m0, double x1, double y1, double m1,
               double x) noexcept {
    const double h = x1 - x0, t = (x - x0) / h, t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * y0 + (t3 - 2 * t2 + t) * h * m0 + (-2 * t3 + 3 * t2) * y1 +
           (t3 - t2) * h * m1;
}

}  // namespace

// Its friend, so read_curve can set a curve's view of the nodes.
struct CurveReader {
    static void set(Curve& c, const Node* nodes, uint32_t size, CurveMode mode,
                    CurveExtend extend) noexcept {
        c = Curve();
        c.nodes_ = nodes;
        c.size_ = size;
        c.mode_ = mode;
        c.extend_ = extend;
    }
};

CurveProblem read_curve(const Values& values, Curve& out) noexcept {
    const CurveNames& n = curve_names();
    CurveMode mode = CurveMode::Monotone;
    CurveExtend extend = CurveExtend::Clamp;
    bool moded = false, extended = false;
    size_t i = 0;
    for (; i < values.size(); ++i) {
        const auto s = values[i].try_sym();
        if (!s) break;
        if (!moded && *s == n.monotone) {
            mode = CurveMode::Monotone;
        } else if (!moded && *s == n.linear) {
            mode = CurveMode::Linear;
        } else if (!moded && *s == n.catmull_rom) {
            mode = CurveMode::CatmullRom;
        } else if (!moded && *s == n.constant) {
            mode = CurveMode::Constant;
        } else if (!extended && *s == n.clamp) {
            extend = CurveExtend::Clamp;
        } else if (!extended && *s == n.extrapolate) {
            extend = CurveExtend::Extrapolate;
        } else if (!extended && *s == n.loop) {
            extend = CurveExtend::Loop;
        } else {
            return {
                "is not an interpolation (monotone, linear, catmull-rom, constant) or an "
                "extension (clamp, extrapolate, loop), or names one twice",
                i};
        }
        const bool is_mode =
            *s == n.monotone || *s == n.linear || *s == n.catmull_rom || *s == n.constant;
        (is_mode ? moded : extended) = true;
    }
    const size_t first = i;
    if (first == values.size()) return {"has no points", first};
    double last = 0;
    for (; i < values.size(); ++i) {
        CurvePoint p;
        if (!point_of(values[i], p)) return {"is not a point: (x y) or (x y in out)", i};
        if (i > first && !(p.x > last)) return {"is not after the point before it in x", i};
        last = p.x;
    }
    CurveReader::set(out, values.nodes().data() + first,
                     static_cast<uint32_t>(values.size() - first), mode, extend);
    return {};
}

CurvePoint Curve::point(size_t i) const noexcept {
    if (points_) return points_[i];
    CurvePoint p;
    (void)point_of(nodes_[i], p);  // read_curve checked it
    return p;
}

double Curve::x_at(size_t i) const noexcept {
    if (points_) return points_[i].x;
    return (*nodes_[i].try_array())[0].try_float().value_or(0);  // read_curve checked it
}

// The slope the interpolation gives at key i, for the cubic modes; a linear curve's is the
// segment's own, which within() takes.
double Curve::computed_slope(size_t i) const noexcept {
    const size_t n = size_;
    if (n < 2) return 0;
    const CurvePoint p = point(i);
    auto secant = [&](size_t a, size_t b) {
        const CurvePoint pa = point(a), pb = point(b);
        return (pb.y - pa.y) / (pb.x - pa.x);
    };
    if (i == 0) return secant(0, 1);
    if (i == n - 1) return secant(n - 2, n - 1);
    const CurvePoint before = point(i - 1), after = point(i + 1);
    if (mode_ == CurveMode::CatmullRom) return (after.y - before.y) / (after.x - before.x);
    // Monotone (Fritsch and Butland): flat at a key that is a peak or a valley, else a
    // weighted harmonic mean of the secants, which keeps each segment within its keys.
    const double h0 = p.x - before.x, h1 = after.x - p.x;
    const double d0 = (p.y - before.y) / h0, d1 = (after.y - p.y) / h1;
    if (d0 * d1 <= 0) return 0;
    return 3 * (h0 + h1) / ((2 * h1 + h0) / d0 + (h1 + 2 * h0) / d1);
}

double Curve::slope_in(size_t i) const noexcept {
    const CurvePoint p = point(i);
    return p.sloped ? p.in : computed_slope(i);
}

double Curve::slope_out(size_t i) const noexcept {
    const CurvePoint p = point(i);
    return p.sloped ? p.out : computed_slope(i);
}

// Within the points: x0 <= x <= xn.
double Curve::within(double x) const noexcept {
    // The last key at or before x.
    size_t lo = 0, hi = size_ - 1;
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (x_at(mid) <= x)
            lo = mid;
        else
            hi = mid - 1;
    }
    const CurvePoint a = point(lo);
    if (lo + 1 == size_ || x == a.x || mode_ == CurveMode::Constant) return a.y;
    const CurvePoint b = point(lo + 1);
    if (mode_ == CurveMode::Linear && !a.sloped && !b.sloped)
        return a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x);
    const double secant = (b.y - a.y) / (b.x - a.x);
    const double m0 = mode_ == CurveMode::Linear && !a.sloped ? secant : slope_out(lo);
    const double m1 = mode_ == CurveMode::Linear && !b.sloped ? secant : slope_in(lo + 1);
    return hermite(a.x, a.y, m0, b.x, b.y, m1, x);
}

// The slope a curve leaves its end key `i` with, for extrapolation.
double Curve::end_slope(size_t i) const noexcept {
    if (mode_ == CurveMode::Constant || size_ < 2) return 0;
    const CurvePoint p = point(i);
    if (mode_ == CurveMode::Linear && !p.sloped) {
        const CurvePoint q = point(i == 0 ? 1 : i - 1);
        return (p.y - q.y) / (p.x - q.x);
    }
    return i == 0 ? slope_in(0) : slope_out(i);
}

double Curve::sample(double x) const noexcept {
    if (size_ == 0) return 0;
    const CurvePoint first = point(0), last = point(size_ - 1);
    if (x >= first.x && x <= last.x) return within(x);
    switch (extend_) {
        case CurveExtend::Clamp: return x < first.x ? first.y : last.y;
        case CurveExtend::Extrapolate:
            return x < first.x ? first.y + end_slope(0) * (x - first.x)
                               : last.y + end_slope(size_ - 1) * (x - last.x);
        case CurveExtend::Loop: {
            const double span = last.x - first.x;
            if (!(span > 0)) return first.y;
            double t = std::fmod(x - first.x, span);
            if (t < 0) t += span;
            return within(first.x + t);
        }
    }
    return 0;
}

void Convert<Curve>::to(const Curve& c, ValueList& out) {
    const CurveNames& n = curve_names();
    switch (c.mode()) {
        case CurveMode::Monotone: break;
        case CurveMode::Linear: out.push(Node(n.linear)); break;
        case CurveMode::CatmullRom: out.push(Node(n.catmull_rom)); break;
        case CurveMode::Constant: out.push(Node(n.constant)); break;
    }
    switch (c.extend()) {
        case CurveExtend::Clamp: break;
        case CurveExtend::Extrapolate: out.push(Node(n.extrapolate)); break;
        case CurveExtend::Loop: out.push(Node(n.loop)); break;
    }
    for (size_t i = 0; i < c.size(); ++i) {
        const CurvePoint p = c.point(i);
        ValueList point;
        // A whole number as an int, as a person writes a key's x: (5 3), not (5.0 3.0).
        auto add = [&point](double v) {
            if (v == std::trunc(v) && std::fabs(v) < 9e15)
                point.push(Node(static_cast<int64_t>(v)));
            else
                point.add(v);
        };
        add(p.x);
        add(p.y);
        if (p.sloped) {
            add(p.in);
            add(p.out);
        }
        out.push(Node(Array::make_from(point.nodes())));
    }
}

}  // namespace baker
