// Tweaks that agree across a change: TweakRegistry::read_consistent.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/parse.hpp>
#include <baker/std_interop.hpp>

#include <atomic>
#include <string>
#include <thread>
#include "dev_only.hpp"
#include "ok.hpp"

using namespace baker;
using baker::test::ok;

namespace {

// A worker reading `a` and `b` under read_consistent until `stop`: how often they disagreed,
// and how many reads it made.
struct Reader {
    std::atomic<bool> stop{false};
    std::atomic<int> disagreed{0};
    std::atomic<int> reads{0};
    std::thread thread;

    Reader(const TweakRegistry& registry, const Tweak<int>& a, const Tweak<int>& b)
        : thread([&] {
              while (!stop.load()) {
                  int x = 0, y = 0;
                  registry.read_consistent([&] {
                      x = a;
                      y = b;
                  });
                  if (x != y) disagreed.fetch_add(1);
                  reads.fetch_add(1);
              }
          }) {}
    ~Reader() {
        stop = true;
        thread.join();
    }
};

}  // namespace

TEST_CASE("read_consistent with no change runs its reader once") {
    TweakRegistry registry;
    Tweak<int> a{"a", 1, {}, registry};
    int runs = 0;
    registry.read_consistent([&] { ++runs; });
    CHECK(runs == 1);
}

TEST_CASE("a worker never sees two tweaks a layer change keeps equal disagree") {
    TweakRegistry registry;
    Tweak<int> a{"pair.a", 0, {}, registry};
    Tweak<int> b{"pair.b", 0, {}, registry};
    Config cfg({.tweaks = &registry});
    REQUIRE(cfg.push_layer_text("pair", "(pair (a 0) (b 0))\n"));
    Reader reader(registry, a, b);
    for (int k = 1; k <= 400; ++k) {
        const std::string text =
            "(pair (a " + std::to_string(k) + ") (b " + std::to_string(k) + "))\n";
        REQUIRE(cfg.set_layer_text("pair", text));
    }
    CHECK(a == 400);
    while (reader.reads.load() < 100) std::this_thread::yield();
    CHECK(reader.disagreed.load() == 0);
}

BAKER_DEV_TEST_CASE("one Config::set that moves two tweaks is seen whole") {
    TweakRegistry registry;
    Tweak<int> lo{"span.0", 0, {}, registry};
    Tweak<int> hi{"span.1", 0, {}, registry};
    Config cfg({.tweaks = &registry});
    REQUIRE(cfg.push_layer("base", ok(read_string("(span 0 0)\n", "t.dta"))));
    Reader reader(registry, lo, hi);
    for (int k = 1; k <= 2000; ++k) {
        const Node both[] = {Node(int64_t{k}), Node(int64_t{k})};
        REQUIRE(cfg.set(Path("span"), std::span<const Node>(both)));
    }
    CHECK(hi == 2000);
    while (reader.reads.load() < 100) std::this_thread::yield();
    CHECK(reader.disagreed.load() == 0);
}
