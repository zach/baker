// Checking a layer off the Config's thread: LayerChecker.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/std_interop.hpp>

#include "temp_dir.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include "ok.hpp"

using namespace baker;
using baker::test::ok;
using baker::test::TempDir;

namespace {

std::string base_text() {
    std::string s =
        "(pack game_data (level game) (version \"1.2\"))\n(origin \"base\" (tool bake))\n(shop\n";
    for (int i = 0; i < 64; ++i)
        s += " (item" + std::to_string(i) + " (price " + std::to_string(i + 1) + "))\n";
    return s + ")\n";
}

const char* const kOverlay =
    "(pack sale (level live) (requires (game_data \"1.2\")))\n"
    "(shop (item3 (price 0)) (item9 (price 5)) (item70 (price 7)))\n";

// A validator that reads only its stack: free items are warned about.
void no_free_items(const ValidatorStack& stack, const Callback<void(const Error&)>& warn) {
    const Array* shop = stack.root.try_find(Symbol("shop"));
    if (!shop || !warn) return;
    for (const Node& n : shop->nodes())
        if (const Array* item = n.try_array(); item && item->get<int>("price").value_or(1) == 0)
            warn(make_error(ErrorCode::Range, "free: " + std::string(item->tag().str())));
}

std::string summary(const LayerReport& r) {
    std::string out;
    for (const LayerEffectEntry& e : r.effect) {
        out += std::string(e.path.str()) + " " + to_string(e.value);
        if (e.below) out += " below " + to_string(*e.below);
        if (e.below_source)
            out += " from " + std::string(e.below_source->layer.str()) + ":" +
                   std::to_string(e.below_source->line) + " " +
                   std::string(e.below_source->origin.str());
        out += "\n";
    }
    for (const Error& e : r.refusals) out += "refused: " + std::string(e.message()) + "\n";
    for (const Error& e : r.warnings) out += "warning: " + std::string(e.message()) + "\n";
    return out;
}

struct Fixture {
    TempDir dir;
    std::unique_ptr<Config> cfg = std::make_unique<Config>(ConfigOptions{.tweaks = nullptr});
    Fixture() {
        cfg->session().set_pack_levels({"game", "live"});
        REQUIRE(cfg->push_layer("default", dir.write("default.dta", base_text())));
        REQUIRE(cfg->push_layer("tuning", dir.write("tuning.dta", "(shop (item9 (price 50)))\n")));
        cfg->add_validator(no_free_items, {.any_thread = true});
    }
};

}  // namespace

TEST_CASE("a checker reports what the Config reports") {
    Fixture f;
    const LayerPolicy policy{.data_only = true, .existing_only = true, .warnings_refuse = true};
    const LayerChecker checker = f.cfg->checker();
    const LayerReport mine = ok(checker.check_text("sale", kOverlay, policy));
    const LayerReport theirs = ok(f.cfg->check_layer_text("sale", kOverlay, policy));
    CHECK(summary(mine) == summary(theirs));
    CHECK(summary(mine).find("from tuning:1") != std::string::npos);
    CHECK(summary(mine).find("from default:7 base") != std::string::npos);
    CHECK(summary(mine).find("refused: a warning refuses the layer: free: item3") !=
          std::string::npos);
    const Ref<Array> tree = ok(read_string("(shop (item1 (price 4)))\n", "t"));
    CHECK(summary(ok(checker.check("t", tree, policy))) ==
          summary(ok(f.cfg->check_layer("t", tree, policy))));
    // It reads text as the Config does: no includes.
    CHECK_FALSE(checker.check_text("sale", "#include default.dta\n"));
    CHECK_FALSE(LayerChecker().check_text("sale", kOverlay));
}

TEST_CASE("a checker outlives its Config, and keeps the state it was made from") {
    Fixture f;
    const LayerChecker checker = f.cfg->checker();
    REQUIRE(f.cfg->remove_layer("tuning"));
    const LayerReport r = ok(checker.check_text("sale", kOverlay));
    CHECK(summary(r).find("item9.price 5 below 50 from tuning") != std::string::npos);
    f.cfg.reset();
    CHECK(summary(ok(checker.check_text("sale", kOverlay))) == summary(r));
}

#if BAKER_ATOMIC_REFCOUNT_ENABLED  // a checker off the Config's thread needs atomic counts
TEST_CASE("a checker checks on another thread while the Config changes") {
    Fixture f;
    const LayerPolicy policy{.data_only = true, .warnings_refuse = true};
    const LayerChecker checker = f.cfg->checker();
    const std::string expected = summary(ok(checker.check_text("sale", kOverlay, policy)));
    std::atomic<bool> done{false};
    std::atomic<int> checks{0}, mismatches{0};
    std::thread worker([&] {
        const LayerChecker mine = checker;  // a copy shares the state
        while (!done.load() || checks.load() < 20) {
            auto r = mine.check_text("sale", kOverlay, policy);
            if (!r || summary(*r) != expected) ++mismatches;
            ++checks;
        }
    });
    for (int i = 0; i < 50; ++i) {
        const std::string name = "hotfix" + std::to_string(i);
        REQUIRE(f.cfg->push_layer_text(name, "(shop (item9 (price " + std::to_string(i) + ")))\n"));
        if constexpr (!kShipping) REQUIRE(f.cfg->set("shop.item1.price", Node(int64_t{i})));
        REQUIRE(f.cfg->remove_layer(name));
    }
    done = true;
    worker.join();
    CHECK(checks.load() >= 20);
    CHECK(mismatches.load() == 0);
}
#endif

TEST_CASE("a checker's report holds until the Config changes, and the push runs the rest") {
    Fixture f;
    std::atomic<int> any_runs{0};  // one the checker runs
    f.cfg->add_validator(
        [&any_runs](const ValidatorStack&, const Callback<void(const Error&)>&) { ++any_runs; },
        {.any_thread = true});
    int owner_runs = 0;  // one it leaves out
    f.cfg->add_validator(
        [&owner_runs](const ValidatorStack& stack, const Callback<void(const Error&)>& warn) {
            ++owner_runs;
            if (warn && stack.root.get<int>("shop.item9.price").value_or(0) == 13)
                warn(make_error(ErrorCode::Range, "13 is unlucky"));
        });
    const LayerPolicy policy{.data_only = true, .warnings_refuse = true};
    const char* const good = "(shop (item9 (price 12)))\n";
    const char* const unlucky = "(shop (item9 (price 13)))\n";

    LayerChecker checker = f.cfg->checker();
    LayerReport r = ok(checker.check_text("hotfix", good, policy));
    CHECK(owner_runs == 0);
    CHECK(any_runs == 1);
    REQUIRE(f.cfg->push_layer_text("hotfix", good, {.policy = &policy, .checked = &r}));
    CHECK(owner_runs == 2);  // the left-out check, then the merge
    CHECK(any_runs == 2);    // the merge alone
    CHECK(f.cfg->get<int>("shop.item9.price") == 12);
    REQUIRE(f.cfg->remove_layer("hotfix"));

    // The checker passes what only the owner's validator refuses; the push refuses it.
    checker = f.cfg->checker();
    r = ok(checker.check_text("hotfix", unlucky, policy));
    CHECK(r.ok());
    const uint64_t epoch = f.cfg->epoch();
    auto pushed = f.cfg->push_layer_text("hotfix", unlucky, {.policy = &policy, .checked = &r});
    REQUIRE_FALSE(pushed);
    CHECK(std::string(pushed.error().message()).find("13 is unlucky") != std::string::npos);
    CHECK(f.cfg->epoch() == epoch);

    // A checker made before a change: its reports do not hold, and the push checks.
    checker = f.cfg->checker();
    r = ok(checker.check_text("hotfix", good, policy));
    REQUIRE(f.cfg->push_layer("x", f.dir.write("x.dta", "(shop (item2 (price 9)))\n")));
    const int before = any_runs;
    REQUIRE(f.cfg->push_layer_text("hotfix", good, {.policy = &policy, .checked = &r}));
    CHECK(any_runs - before == 2);  // the full check, then the merge
}
