// Live ops: a shipped build takes published overlays while it runs. Each is staged
// (checked, changing nothing), applied under the same policy at a moment the game chooses,
// and rolled back with one call.
//
//   baker_example_live_ops path/to/examples/live-ops
//
// The overlays are files here, standing in for the storage a game fetches them from; the
// game reads each as text it received, and names the layer after it. An overlay is a pack:
// it names itself, its version, and the base it was made for.
#include <baker/config.hpp>
#include <baker/shape.hpp>
#include <baker/std_interop.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static baker::Tweak<float> kXp{"event.xp_multiplier", 1.0f};
static baker::Tweak<float> kDrops{"event.drop_rate", 0.05f};

namespace {

void status(const baker::Config& cfg) {
    auto banner = cfg.get<std::string_view>("event.banner", "");
    std::printf("  xp x%g, drops %g, banner \"%.*s\"\n", double(kXp), double(kDrops),
                int(banner ? banner->size() : 0), banner ? banner->data() : "");
}

// What a live overlay may do in this game: change data under (event ...), to plain values,
// with every validator's warning a refusal, so an overlay made for another base, or one the
// shapes reject, never applies.
const baker::Path kEvent[] = {baker::Path("event")};
const baker::LayerPolicy kLive{
    .data_only = true, .scalars_only = true, .warnings_refuse = true, .allow = kEvent};

std::string text_of(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// Stage: check the overlay as it arrived, changing nothing. A game does this as soon as an
// overlay downloads, and logs or shows the report; it keeps the report for the push.
bool stage(const baker::Config& cfg, const std::string& name, baker::LayerReport& staged) {
    std::printf("stage %s\n", name.c_str());
    const std::string file = "overlays/" + name + ".dta";
    auto report = cfg.check_layer_text(file, text_of(file), kLive);
    if (!report) return std::printf("  %s\n", baker::to_string(report.error()).data()), false;
    staged = *report;
    for (const baker::Error& e : report->refusals)
        std::printf("  refused: %s\n", baker::to_string(e).data());
    if (!report->ok()) return false;
    for (const baker::LayerEffectEntry& e : report->effect)
        std::printf("  %s: %s -> %s\n", e.path.c_str(),
                    e.below ? baker::to_string(*e.below).c_str() : "(none)",
                    baker::to_string(e.value).c_str());
    return true;
}

// Apply: at a boundary the game chooses (a session's start, a lobby), push the text under
// the same policy, with the staged report. If nothing changed since the stage, the push takes
// its verdict, costing what an unchecked push costs; if the stack moved, it checks again and
// refuses, changing nothing, if the policy does.
bool apply(baker::Config& cfg, const std::string& name, const baker::LayerReport& staged) {
    std::printf("apply %s\n", name.c_str());
    const std::string file = "overlays/" + name + ".dta";
    if (auto r = cfg.push_layer_text(file, text_of(file), {.policy = &kLive, .checked = &staged});
        !r)
        return std::printf("  %s\n", baker::to_string(r.error()).data()), false;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::filesystem::current_path(argv[1]);  // so errors name files as the studio does

    // The environment boundary: a shipped build loads its data, then freezes. Only layers
    // can change after that, and only because live_layers says so: no reloads of the
    // shipped files and no live edits.
    baker::Config cfg({.live_layers = true});
    cfg.session().set_pack_levels({"game", "live"});
    if (!cfg.push_layer("default", "data/default.dta")) return 1;
    cfg.add_validator(baker::shape::validator(),
                      {.any_thread = true});  // the shapes judge every candidate
    cfg.freeze();
    std::printf("shipped:\n");
    status(cfg);

    const std::string xp = "2026-10-17-double-xp";
    baker::LayerReport staged;
    if (!stage(cfg, xp, staged) || !apply(cfg, xp, staged)) return 1;
    status(cfg);
    // Provenance names the overlay, and the tool that published it.
    if (auto s = cfg.source("event.xp_multiplier"))
        std::printf("  event.xp_multiplier from layer %s (%s, by %s)\n", s->layer.c_str(),
                    s->origin.c_str(), s->tool.c_str());

    // Three that never reach players: a typo the shapes reject, an overlay made for a newer
    // base, and one that runs script.
    for (const char* bad : {"2026-10-18-drops", "2026-10-19-season-two", "2026-10-20-lucky-hour"})
        if (stage(cfg, bad, staged)) return 1;
    status(cfg);

    // Rollback: the layer goes, and the values below it return. Nothing is left behind.
    std::printf("roll back %s\n", xp.c_str());
    if (!cfg.remove_layer("overlays/" + xp + ".dta")) return 1;
    status(cfg);
    std::printf("layers:");
    for (const std::string& name : baker::strings(cfg.layers())) std::printf(" %s", name.c_str());
    std::printf("\n");
    return 0;
}
