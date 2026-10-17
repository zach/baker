// Taking a layer from outside: checking it and pushing it. The case is a live
// overlay: about 10 KB changing 400 prices of a 2,000-item table (155 KB), under a policy
// with every rule on. Each row is the best of many runs, in microseconds.
#include <baker/config.hpp>
#if defined(BAKER_BENCH_SHAPE)
#include <baker/shape.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

namespace {

std::string base_text() {
    std::string s = "(shop\n";
    for (int i = 0; i < 2000; ++i)
        s += "   (item" + std::to_string(i) + " (price " + std::to_string(10 + i % 90) +
             ") (damage " + std::to_string(i % 7) + ") (name \"Item number " + std::to_string(i) +
             "\"))\n";
    s += ")\n";
#if defined(BAKER_BENCH_SHAPE)
    s += "(shapes (item (at shop) (keyed) (price int (range 0 1000)) (damage int) (name "
         "string)))\n";
#endif
    return s;
}

std::string overlay_text() {
    std::string s = "(shop\n";
    for (int i = 0; i < 2000; i += 5)
        s += "   (item" + std::to_string(i) + " (price " + std::to_string(20 + i % 90) + "))\n";
    return s + ")\n";
}

template <class F>
double best_us(int runs, F&& f) {
    double best = 1e30;
    for (int i = 0; i < runs; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    return best;
}

void row(const char* name, double us) {
    std::printf("%-44s %9.1f us\n", name, us);
}

}  // namespace

int main() {
    const std::string base = base_text(), overlay = overlay_text();
    std::printf("base %zu bytes, overlay %zu bytes\n", base.size(), overlay.size());
    baker::Config cfg({.edit_mode = false, .tweaks = nullptr});
    if (!cfg.push_layer_text("default", base)) return 1;
#if defined(BAKER_BENCH_SHAPE)
    cfg.add_validator(baker::shape::validator(), {.any_thread = true});
    std::printf("with shape::validator\n");
#endif
    const baker::Path shop[] = {baker::Path("shop")};
    const baker::LayerPolicy policy{.data_only = true,
                                    .scalars_only = true,
                                    .existing_only = true,
                                    .warnings_refuse = true,
                                    .allow = shop};
    constexpr int kRuns = 200;
    bool ok = true;

    row("read_string",
        best_us(kRuns, [&] { ok = ok && baker::read_string(overlay, "overlay").has_value(); }));
    const baker::Ref<baker::Array> tree = *baker::read_string(overlay, "overlay");
    row("check_layer (tree)",
        best_us(kRuns, [&] { ok = ok && cfg.check_layer("overlay", tree, policy).has_value(); }));
    row("check_layer_text", best_us(kRuns, [&] {
            ok = ok && cfg.check_layer_text("overlay", overlay, policy).has_value();
        }));

    // Each push is timed alone; removing the layer after it is not.
    auto pushes = [&](const char* name, auto&& push) {
        double best = 1e30;
        for (int i = 0; i < kRuns; ++i) {
            best = std::min(best, best_us(1, [&] { ok = ok && push(); }));
            ok = ok && cfg.remove_layer("overlay").has_value();
        }
        row(name, best);
    };
    pushes("push_layer (tree), no policy",
           [&] { return cfg.push_layer("overlay", tree).has_value(); });
    pushes("push_layer (tree), policy",
           [&] { return cfg.push_layer("overlay", tree, {.policy = &policy}).has_value(); });
    pushes("push_layer_text, no policy",
           [&] { return cfg.push_layer_text("overlay", overlay).has_value(); });
    pushes("push_layer_text, policy", [&] {
        return cfg.push_layer_text("overlay", overlay, {.policy = &policy}).has_value();
    });
    // With a report from a check just before (untimed), which still holds.
    baker::LayerReport report;
    auto held = [&](const char* name, auto&& check, auto&& push) {
        double best = 1e30;
        for (int i = 0; i < kRuns; ++i) {
            report = *check();
            best = std::min(best, best_us(1, [&] { ok = ok && push(); }));
            ok = ok && cfg.remove_layer("overlay").has_value();
        }
        row(name, best);
    };
    held(
        "push_layer (tree), policy, report held",
        [&] { return cfg.check_layer("overlay", tree, policy); },
        [&] {
            return cfg.push_layer("overlay", tree, {.policy = &policy, .checked = &report})
                .has_value();
        });
    held(
        "push_layer_text, policy, report held",
        [&] { return cfg.check_layer_text("overlay", overlay, policy); },
        [&] {
            return cfg.push_layer_text("overlay", overlay, {.policy = &policy, .checked = &report})
                .has_value();
        });
    // Off the Config's thread: taking a checker is the owning thread's part.
    row("checker()", best_us(kRuns, [&] { ok = ok && !cfg.checker().empty(); }));
    const baker::LayerChecker checker = cfg.checker();
    row("LayerChecker::check_text", best_us(kRuns, [&] {
            ok = ok && checker.check_text("overlay", overlay, policy).has_value();
        }));
    held(
        "push_layer_text, policy, checker's report",
        [&] { return cfg.checker().check_text("overlay", overlay, policy); },
        [&] {
            return cfg.push_layer_text("overlay", overlay, {.policy = &policy, .checked = &report})
                .has_value();
        });
    if (!ok) std::printf("FAILED\n");
    return ok ? 0 : 1;
}
