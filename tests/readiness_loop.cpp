// The data-layer rows of a studio's readiness gate, end to end (spec M3b, Change 4):
// edit-to-see measured with Baker's share visible, a tuned value saved as a one-line diff,
// an experiment compared A/B without touching shared files, and a return to an exact state.
#include <doctest.h>
#include <baker/config.hpp>
#include <baker/std_interop.hpp>

#include "alloc_counter.hpp"
#include "temp_dir.hpp"

#include <string>
#include <vector>

using namespace baker;

#if !BAKER_SHIPPING
namespace {

struct FakeClock {
    uint64_t t = 1'000'000;
    static uint64_t tick(void* self) { return static_cast<FakeClock*>(self)->t += 250; }
};

// Reading a tweak allocates nothing.
size_t read_allocations(const Tweak<float>& tweak) {
    test::AllocationScope scope;
    float sum = 0;
    for (int i = 0; i < 1000; ++i) sum += tweak;
    CHECK(sum > 0);
    return scope.count();
}

}  // namespace

TEST_CASE("readiness loop: edit, see, tune, save, experiment, and return") {
    test::TempDir dir;
    const bool counting = test::counting_allocations();

    // 1. A commented tunable with a note, and a personal layer whose file does not exist.
    const std::string shared =
        "; boss tuning, shared by the encounter team\n"
        "(warden\n"
        "   (cadence 1.8 (note \"feels unfair below 1.0 on handheld\"))   ; seconds\n"
        "   (hp 1200))\n";
    dir.write("default.dta", shared);
    TweakRegistry reg;
    // 2. A tweak bound to the tunable.
    Tweak<float> cadence{"warden.cadence", 2.0f, {.range = {0.2, 5}}, reg};
    Config cfg({.tweaks = &reg});
    REQUIRE(cfg.push_layer("default", dir.path("default.dta")));
    REQUIRE(cfg.push_layer("local", dir.path("local/designer.dta"), {.optional = true}));
    CHECK(float(cadence) == 1.8f);
    CHECK(cadence.note() == "feels unfair below 1.0 on handheld");
    if (counting) CHECK(read_allocations(cadence) == 0);

    // 3. Edit on disk; reload with the save time as the origin, under a fake clock.
    FakeClock clock;
    cfg.set_time_source(FakeClock::tick, &clock);
    std::string edited = shared;
    edited.replace(edited.find("1.8"), 3, "1.5");
    dir.write("default.dta", edited);
    const uint64_t saved_at = clock.t;
    REQUIRE(cfg.reload({.trigger = "test"_sym, .origin_ns = saved_at}) == true);
    CHECK(float(cadence) == 1.5f);
    const auto rec = cfg.last_reload();
    REQUIRE(rec);
    CHECK(rec->trigger == Symbol("test"));
    CHECK(rec->outcome == ReloadOutcome::Ok);
    CHECK(rec->origin_ns == saved_at);
    CHECK(rec->start_ns > rec->origin_ns);
    CHECK(rec->scan_ns > 0);
    CHECK(rec->read_ns > 0);
    CHECK(rec->parse_ns > 0);
    CHECK(rec->merge_ns > 0);
    CHECK(rec->bind_ns > 0);
    CHECK(rec->notify_ns > 0);
    CHECK(rec->write_ns == 0);  // a reload writes nothing
    CHECK(rec->scan_ns + rec->read_ns + rec->parse_ns + rec->merge_ns + rec->bind_ns +
              rec->notify_ns ==
          rec->end_ns - rec->start_ns);
    CHECK(rec->end_ns - rec->origin_ns > 0);  // Baker's share of edit-to-see
    CHECK(rec->files_reloaded == 1);
    CHECK(rec->tweaks_changed == 1);
    CHECK(rec->first_file.str().ends_with("default.dta"));
    CHECK(rec->epoch == cfg.epoch());

    // 4. Tune in play, then save: a one-line diff with the comment and note intact.
    REQUIRE(cfg.set("warden.cadence", Node(1.25)));
    CHECK(float(cadence) == 1.25f);
    CHECK(cfg.source("warden.cadence")->layer == "scratch");
    REQUIRE(cfg.write_back("warden.cadence"));
    const std::string saved = dir.read("default.dta");
    CHECK(test::diff_lines(edited, saved) ==
          std::vector<std::string>{
              "-   (cadence 1.5 (note \"feels unfair below 1.0 on handheld\"))   ; seconds",
              "+   (cadence 1.25 (note \"feels unfair below 1.0 on handheld\"))   ; seconds"});
    CHECK(cfg.unsaved().empty());
    const Source source = *cfg.source("warden.cadence");
    CHECK(source.layer == "default");
    CHECK(source.line == 3);

    // 5. An experiment layer, compared A/B by toggling, without touching shared files.
    REQUIRE(cfg.push_layer("experiment", dir.write("experiment.dta", "(warden (cadence 0.9))\n")));
    CHECK(float(cadence) == 0.9f);
    const auto effect = cfg.layer_effect("experiment");
    REQUIRE(effect);
    REQUIRE(effect->size() == 1);
    CHECK((*effect)[0].path == "warden.cadence");
    CHECK((*effect)[0].value.try_float() == 0.9);
    CHECK((*effect)[0].below->try_float() == 1.25);
    CHECK((*effect)[0].below_source->layer == "default");
    CHECK_FALSE((*effect)[0].shadowed);
    CHECK(cfg.toggle_layer("experiment") == false);
    CHECK(float(cadence) == 1.25f);  // variant A
    CHECK(cfg.toggle_layer("experiment") == true);
    CHECK(float(cadence) == 0.9f);  // variant B
    CHECK(dir.read("default.dta") == saved);

    // 6. Snapshot, disturb the stack, and come back exactly.
    const ConfigSnapshot snap = cfg.snapshot();
    const Ref<Array> before = cfg.root();
    REQUIRE(cfg.toggle_layer("experiment"));
    REQUIRE(cfg.push_layer("patch", dir.write("patch.dta", "(warden (hp 900))\n")));
    REQUIRE(cfg.set("warden.cadence", Node(3.0)));
    REQUIRE(cfg.restore(snap));
    CHECK(structurally_equal(*cfg.root(), *before));
    CHECK(float(cadence) == 0.9f);
    CHECK(cfg.source("warden.cadence")->layer == "experiment");
    CHECK(strings(cfg.layers()) ==
          std::vector<std::string>{"default", "local", "experiment", "scratch"});
    CHECK(cfg.last_reload()->trigger == Symbol("restore"));

    // 7. Reload telemetry and tweak reads allocate nothing.
    if (counting) {
        CHECK(read_allocations(cadence) == 0);
        auto reload_allocations = [&](size_t history) {
            cfg.set_reload_history(history);
            for (int i = 0; i < 4; ++i) (void)cfg.reload();
            test::AllocationScope scope;
            for (int i = 0; i < 16; ++i) (void)cfg.reload();
            return scope.count();
        };
        CHECK(reload_allocations(4) == reload_allocations(0));
    }
}
#endif
