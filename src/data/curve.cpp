// Curves: reading one from a key's values, and sampling it; the baked copies a
// load keeps of the curves sampled from it.

#include <baker/curve.hpp>

#include "arena.hpp"
#include "curve_access.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>

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
    const auto count = static_cast<uint32_t>(values.size() - first);
    // Values that are an array's own can be baked by its load; a copy already baked was
    // checked when it was made, and the load's arrays have not changed since.
    const Array* source = values.source();
    if (source &&
        reinterpret_cast<uintptr_t>(values.begin()) >=
            reinterpret_cast<uintptr_t>(source->begin()) &&
        reinterpret_cast<uintptr_t>(values.end()) <= reinterpret_cast<uintptr_t>(source->end())) {
        if (const CurvePoint* baked =
                detail::baked_curve(*source, values.begin() + first, count, false)) {
            out = Curve(std::span<const CurvePoint>(baked, count), mode, extend);
            return {};
        }
    } else {
        source = nullptr;
    }
    double last = 0;
    for (; i < values.size(); ++i) {
        CurvePoint p;
        if (!point_of(values[i], p)) return {"is not a point: (x y) or (x y in out)", i};
        if (i > first && !(p.x > last)) return {"is not after the point before it in x", i};
        last = p.x;
    }
    CurveReader::set(out, values.begin() + first, count, mode, extend, source);
    return {};
}

CurvePoint Curve::point(size_t i) const noexcept {
    if (!nodes_) return points_[i];
    CurvePoint p;
    (void)point_of(nodes_[i], p);  // read_curve checked it
    return p;
}

double Curve::x_at(size_t i) const noexcept {
    if (!nodes_) return points_[i].x;
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
    if (sourced_) {
        // Read from a load: sample its baked copy, made by the first sample.
        if (const CurvePoint* baked = detail::baked_curve(*source_, nodes_, size_, true))
            return Curve(std::span<const CurvePoint>(baked, size_), mode_, extend_).sample(x);
    }
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

namespace detail {

// The baked curves of one load, made at the first sample of a curve read from it and
// freed with it. A fixed table of slots, each filled once by a compare-and-swap and never
// emptied until the load is freed, so a reader finds a copy with acquire loads alone and
// no copy is freed while a Curve may view it. Bounded: at most kMaxEntries copies and
// kMaxPoints points; a curve past either samples from its nodes.
struct CurveEntry {
    const Node* nodes;  // the key: the first point's node, and the count
    uint32_t size;
    uint32_t epoch;  // the cache's epoch when baked; a copy of another is stale
    // CurvePoint points[size] follows.
    [[nodiscard]] CurvePoint* points() noexcept { return reinterpret_cast<CurvePoint*>(this + 1); }
    [[nodiscard]] static size_t bytes(uint32_t size) noexcept {
        return sizeof(CurveEntry) + size_t{size} * sizeof(CurvePoint);
    }
};
static_assert(sizeof(CurveEntry) % alignof(CurvePoint) == 0);

struct CurveCache {
    static constexpr uint32_t kSlots = 128;  // twice the entries, so a probe stays short
    static constexpr uint32_t kMaxEntries = 64;
    static constexpr uint32_t kMaxPoints = 8192;
    std::atomic<uint32_t> epoch{0};  // moved by Array::set on the load's arrays
    std::atomic<uint32_t> entries{0};
    std::atomic<uint32_t> points{0};
    std::atomic<CurveEntry*> slots[kSlots] = {};
};

namespace {

uint32_t slot_of(const Node* nodes) noexcept {
    const auto p = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(nodes));
    return static_cast<uint32_t>((p >> 4) * 0x9E3779B97F4A7C15ull >> 57);  // 7 bits: 128
}
static_assert(CurveCache::kSlots == 128);

// Takes `n` from `count` if it stays within `max`.
bool reserve(std::atomic<uint32_t>& count, uint32_t n, uint32_t max) noexcept {
    uint32_t now = count.load(std::memory_order_relaxed);
    do {
        if (n > max - now) return false;
    } while (!count.compare_exchange_weak(now, now + n, std::memory_order_relaxed));
    return true;
}

}  // namespace

const CurvePoint* baked_curve(const Array& source, const Node* nodes, uint32_t size,
                              bool bake) noexcept {
    Owner& owner = owner_of(&source);
    if (owner.kind != kArena) return nullptr;
    std::atomic<CurveCache*>& home = Arena::of(owner).curves();
    CurveCache* cache = home.load(std::memory_order_acquire);
    if (!cache) {
        if (!bake) return nullptr;
        auto* made =
            ::new (default_alloc(sizeof(CurveCache), alignof(CurveCache), MemoryKind::Trees))
                CurveCache;
        if (home.compare_exchange_strong(cache, made, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            cache = made;
        } else {  // another thread made it first: `cache` is now its
            made->~CurveCache();
            default_free(made, sizeof(CurveCache), MemoryKind::Trees);
        }
    }
    const uint32_t epoch = cache->epoch.load(std::memory_order_relaxed);
    for (;;) {
        uint32_t empty = CurveCache::kSlots;
        uint32_t at = slot_of(nodes);
        for (uint32_t probe = 0; probe < CurveCache::kSlots; ++probe) {
            CurveEntry* e = cache->slots[at].load(std::memory_order_acquire);
            if (!e) {
                empty = at;
                break;
            }
            if (e->nodes == nodes && e->size == size && e->epoch == epoch) return e->points();
            at = (at + 1) % CurveCache::kSlots;
        }
        if (!bake || empty == CurveCache::kSlots) return nullptr;
        if (!reserve(cache->entries, 1, CurveCache::kMaxEntries)) return nullptr;
        if (!reserve(cache->points, size, CurveCache::kMaxPoints)) {
            cache->entries.fetch_sub(1, std::memory_order_relaxed);
            return nullptr;
        }
        auto* e = static_cast<CurveEntry*>(
            default_alloc(CurveEntry::bytes(size), alignof(CurveEntry), MemoryKind::Trees));
        e->nodes = nodes;
        e->size = size;
        e->epoch = epoch;
        CurvePoint* p = e->points();
        for (uint32_t i = 0; i < size; ++i) {
            ::new (p + i) CurvePoint;
            (void)point_of(nodes[i], p[i]);  // read_curve checked them
        }
        CurveEntry* expected = nullptr;
        if (cache->slots[empty].compare_exchange_strong(expected, e, std::memory_order_acq_rel,
                                                        std::memory_order_acquire))
            return p;
        // Another thread filled the slot first, perhaps with this curve: probe again.
        default_free(e, CurveEntry::bytes(size), MemoryKind::Trees);
        cache->entries.fetch_sub(1, std::memory_order_relaxed);
        cache->points.fetch_sub(size, std::memory_order_relaxed);
    }
}

void curves_changed(CurveCache& cache) noexcept {
    cache.epoch.fetch_add(1, std::memory_order_relaxed);
}

void free_curves(CurveCache* cache) noexcept {
    for (std::atomic<CurveEntry*>& slot : cache->slots)
        if (CurveEntry* e = slot.load(std::memory_order_relaxed))
            default_free(e, CurveEntry::bytes(e->size), MemoryKind::Trees);
    cache->~CurveCache();
    default_free(cache, sizeof(CurveCache), MemoryKind::Trees);
}

}  // namespace detail

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
