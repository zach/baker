// A curve as a value: reading, sampling each interpolation and extension, tweaks.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/curve.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>
#include <baker/tweak.hpp>

#include <string>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;

namespace {

// The curve `text`'s key `name` holds, with the tree it views.
struct Read {
    Ref<Array> tree;
    Curve curve;
    explicit Read(const std::string& text) {
        tree = ok(read_string(text, "t.dta"));
        const Array* a = (*tree)[0].try_array();
        REQUIRE(a);
        REQUIRE_FALSE(read_curve(a->values(), curve));
    }
};

std::string problem(const std::string& text) {
    const auto tree = ok(read_string(text, "t.dta"));
    Curve c;
    const CurveProblem p = read_curve((*tree)[0].try_array()->values(), c);
    return p ? p.what : "";
}

}  // namespace

TEST_CASE("each interpolation at its keys, between them, and beyond them") {
    const std::string points = " (0 0) (10 10) (20 0))";
    const Read linear("(c linear" + points), constant("(c constant" + points),
        catmull("(c catmull-rom" + points), monotone("(c" + points);
    CHECK(monotone.curve.mode() == CurveMode::Monotone);
    for (const Read* r : {&linear, &constant, &catmull, &monotone}) {
        CHECK(r->curve.size() == 3);
        CHECK(r->curve.sample(0) == 0);  // at each key, its y
        CHECK(r->curve.sample(10) == 10);
        CHECK(r->curve.sample(20) == 0);
        CHECK(r->curve.sample(-5) == 0);  // clamped
        CHECK(r->curve.sample(99) == 0);
    }
    CHECK(linear.curve.sample(5) == doctest::Approx(5));
    CHECK(linear.curve.sample(15) == doctest::Approx(5));
    CHECK(constant.curve.sample(5) == 0);  // held to the next key
    CHECK(constant.curve.sample(15) == 10);
    // Hermite, from the end key's secant (1) to the peak's flat slope.
    CHECK(catmull.curve.sample(5) == doctest::Approx(6.25));
    CHECK(monotone.curve.sample(5) == doctest::Approx(6.25));

    // Beyond the points: on along the end slopes, or round again.
    const Read extrapolated("(c linear extrapolate" + points), looped("(c loop linear" + points);
    CHECK(extrapolated.curve.extend() == CurveExtend::Extrapolate);
    CHECK(extrapolated.curve.sample(-5) == doctest::Approx(-5));
    CHECK(extrapolated.curve.sample(25) == doctest::Approx(-5));
    CHECK(looped.curve.sample(25) == doctest::Approx(5));  // as at 5
    CHECK(looped.curve.sample(-5) == doctest::Approx(5));  // as at 15
    CHECK(looped.curve.sample(40) == doctest::Approx(0));  // as at 0
    const Read flat("(c constant extrapolate (0 1) (1 2))");
    CHECK(flat.curve.sample(5) == 2);  // a constant curve has no slope to go on along

    // One point is a constant; no curve samples 0.
    CHECK(Read("(c (3 7))").curve.sample(-100) == 7);
    CHECK(Curve().sample(1) == 0);
}

TEST_CASE("monotone never leaves the keys around it; catmull-rom overshoots") {
    const std::string points = " (0 0) (1 0) (2 10) (3 10) (4 2))";
    const Read monotone("(c" + points), catmull("(c catmull-rom" + points);
    double highest = 0, lowest = 10, catmull_highest = 0;
    for (int i = 0; i <= 400; ++i) {
        const double x = i / 100.0;
        highest = std::max(highest, monotone.curve.sample(x));
        lowest = std::min(lowest, monotone.curve.sample(x));
        catmull_highest = std::max(catmull_highest, catmull.curve.sample(x));
    }
    CHECK(highest <= 10);
    CHECK(lowest >= 0);
    CHECK(catmull_highest > 10.1);
    // Within each segment, between that segment's keys.
    for (int i = 0; i <= 100; ++i) {
        const double y = monotone.curve.sample(3 + i / 100.0);
        CHECK(y <= 10);
        CHECK(y >= 2);
    }
}

TEST_CASE("a point's own slopes replace its interpolation's") {
    const Read r("(c linear (0 0) (10 10 0 0) (20 20))");
    CHECK(r.curve.point(1).sloped);
    CHECK(r.curve.sample(5) == doctest::Approx(6.25));    // flat as it arrives at 10
    CHECK(r.curve.sample(15) == doctest::Approx(13.75));  // and flat as it leaves
    const Read plain("(c linear (0 0) (10 10) (20 20))");
    CHECK(plain.curve.sample(5) == doctest::Approx(5));
    // In and out apart: a sharp peak, where monotone alone is flat.
    const Read kink("(c (0 0) (10 10 2 -2) (20 0))"), round("(c (0 0) (10 10) (20 0))");
    CHECK(kink.curve.sample(9.9) < 9.9);
    CHECK(kink.curve.sample(10.1) < 9.9);
    CHECK(round.curve.sample(9.9) > 9.9);
}

TEST_CASE("values that are not a curve say why") {
    CHECK(problem("(c (0 1) (5 3) (10 8))").empty());
    CHECK(problem("(c (0 1) (5 3) (4 8))") == "is not after the point before it in x");
    CHECK(problem("(c (0 1) (0 3))") == "is not after the point before it in x");
    CHECK(problem("(c wobbly (0 1))").find("is not an interpolation") == 0);
    CHECK(problem("(c linear constant (0 1))").find("names one twice") != std::string::npos);
    CHECK(problem("(c (0 1) (5))") == "is not a point: (x y) or (x y in out)");
    CHECK(problem("(c (0 1 2))") == "is not a point: (x y) or (x y in out)");
    CHECK(problem("(c (0 fast))") == "is not a point: (x y) or (x y in out)");
    CHECK(problem("(c linear)") == "has no points");
    CHECK(problem("(c loop catmull-rom (0 1))").empty());  // either order
}

TEST_CASE("a curve writes back as it is read, and a host builds one of its own") {
    const CurvePoint points[] = {{0, 1}, {2.5, 3, 0.5, -1, true}};
    const Curve built(points, CurveMode::Linear, CurveExtend::Loop);
    // Hermite: the segment's secant at 0, the given 0.5 arriving at 2.5.
    CHECK(built.sample(1.25) == doctest::Approx(2.09375));
    ValueList out;
    Convert<Curve>::to(built, out);
    std::string text;
    for (const Node& n : out.nodes()) text += (text.empty() ? "" : " ") + to_string(n);
    CHECK(text == "linear loop (0 1) (2.5 3 0.5 -1)");
    const Read again("(c " + text + ")");
    CHECK(again.curve.sample(1.25) == doctest::Approx(built.sample(1.25)));
    // The defaults are left unwritten.
    ValueList plain;
    Convert<Curve>::to(Read("(c (0 1) (1 2))").curve, plain);
    CHECK(plain.nodes().size() == 2);
}

TEST_CASE("Tweak<Curve> reads a key's curve; a layer replaces it whole") {
    TweakRegistry reg;
    static const CurvePoint fallback[] = {{0, 0}, {1, 1}};
    Tweak<Curve> falloff("falloff", Curve(fallback), {}, reg);
    CHECK(falloff.get().sample(0.5) == doctest::Approx(0.5));
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("base", ok(read_string("(falloff extrapolate (0 1) (10 3))", "b"))));
    CHECK(falloff.get().extend() == CurveExtend::Extrapolate);
    CHECK(falloff.get().sample(5) == doctest::Approx(2));
    CHECK(falloff.get().sample(20) == doctest::Approx(5));
    CHECK(cfg.get<Curve>("falloff")->sample(5) == doctest::Approx(2));
    // A layer's curve: its points, interpolation and extension, nothing of the one below.
    REQUIRE(cfg.push_layer("mod", ok(read_string("(falloff constant (0 5))", "m"))));
    CHECK(falloff.get().mode() == CurveMode::Constant);
    CHECK(falloff.get().extend() == CurveExtend::Clamp);
    CHECK(falloff.get().size() == 1);
    CHECK(falloff.get().sample(20) == 5);
    // Data that is not a curve leaves the tweak as it was, and check_tweaks says why.
    TweakRegistry other;
    Tweak<Curve> broken("broken", Curve(fallback), {.range = {0, 4}}, other);
    const auto tree = ok(read_string("(broken (0 1) (5 9))", "t.dta"));
    TweakFindings f = check_tweaks(*tree, other);
    REQUIRE(f.errors.size() == 1);  // y 9 outside 0 to 4; x 5 is not ranged
    CHECK(to_string(f.errors[0]).find("is 9, outside its tweak's range 0.0 to 4.0") !=
          std::string::npos);
    const auto unordered = ok(read_string("(broken (5 1) (0 2))", "t.dta"));
    CHECK(check_tweaks(*unordered, other).errors.size() == 1);
    // The tweaks JSON names the type, for a panel to draw a curve.
    std::string json;
    write_json(reg, string_out(json));
    CHECK(json.find(R"j("path": "falloff", "type": "curve")j") != std::string::npos);
    if constexpr (!kShipping) {
        const CurvePoint edited[] = {{0, 2}, {4, 6}};
        REQUIRE(falloff.set(Curve(edited, CurveMode::Linear)));
        CHECK(falloff.get().sample(1) == doctest::Approx(3));
        CHECK(cfg.get<Curve>("falloff")->mode() == CurveMode::Linear);
    }
}
