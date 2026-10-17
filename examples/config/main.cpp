// Layers, provenance, reload callbacks, and write-back.
//
//   baker_example_config path/to/examples/config/data
//
// Works on a copy of the data in ./work, so write-back never touches the sources.
#include <baker/config.hpp>
#include <baker/std_interop.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

static baker::Tweak<float> kVolume{"audio.volume", 1.0f};
static baker::Tweak<int> kShadows{"renderer.shadows", 1};

namespace {

void where(baker::Config& cfg, const char* path) {
    auto s = cfg.source(path);
    if (!s) return (void)std::printf("%-17s %s\n", path, baker::to_string(s.error()).data());
    auto file = fs::path(s->file.str()).filename().string();
    if (file.empty())
        std::printf("%-17s layer %-8s (unsaved)\n", path, s->layer.c_str());
    else
        std::printf("%-17s layer %-8s %s:%d\n", path, s->layer.c_str(), file.c_str(), s->line);
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void write(const fs::path& p, const std::string& text) {
    std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

bool ok(const baker::Expected<void>& r) {
    if (!r) std::printf("%s\n", baker::to_string(r.error()).data());
    return bool(r);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    fs::remove_all("work");
    fs::create_directories("work");
    for (const char* f : {"default.dta", "handheld.dta"})
        fs::copy_file(fs::path(argv[1]) / f, fs::path("work") / f);

    // Layers merge bottom to top. The local layer is each developer's own: optional, so a
    // missing file is an empty layer, and kept out of version control.
    baker::Config cfg;
    if (!ok(cfg.push_layer("default", "work/default.dta")) ||
        !ok(cfg.push_layer("handheld", "work/handheld.dta")) ||
        !ok(cfg.push_layer("local", "work/local/sam.dta", {.optional = true})))
        return 1;
    std::printf("layers:");
    for (const std::string& name : baker::strings(cfg.layers())) std::printf(" %s", name.c_str());
    std::printf("\nshadows %d, volume %g\n", int(kShadows), double(kVolume));
    where(cfg, "renderer.shadows");
    where(cfg, "audio.volume");

    // A subsystem rebuilds when its own group changes, and only then.
    cfg.on_reload("audio", [] { std::printf("audio rebuilt: volume %g\n", double(kVolume)); });
    cfg.on_reload("renderer", [] { std::printf("renderer rebuilt\n"); });

    // Someone edits default.dta and saves; the host reloads (a file watcher, or each frame).
    std::string text = read("work/default.dta");
    text.replace(text.find("volume 0.8"), 10, "volume 0.6");
    write("work/default.dta", text);
    if (auto r = cfg.reload(); !r || !*r) return 1;
    if (auto rec = cfg.last_reload())
        std::printf("reload: %u files reread, %u tweak changed\n", rec->files_reloaded,
                    rec->tweaks_changed);

    // Tuned live: the value lives in the scratch layer until it is saved.
    if (!ok(kVolume.set(0.45f))) return 1;
    where(cfg, "audio.volume");
    // Saved where it came from: only the number changes; the comment and metadata stay.
    if (!ok(kVolume.write_back())) return 1;
    where(cfg, "audio.volume");
    std::istringstream lines(read("work/default.dta"));
    for (std::string line; std::getline(lines, line);)
        if (line.find("(volume") != std::string::npos)
            std::printf("default.dta: %s\n", line.c_str());

    // Kept for one developer: written into the local layer, which creates the file.
    if (!ok(cfg.set("audio.channels", 8))) return 1;
    if (auto r = cfg.write_back_to("audio.channels", "local"); !r) {
        std::printf("%s\n", baker::to_string(r.error()).data());
        return 1;
    }
    where(cfg, "audio.channels");
    std::printf("local/sam.dta: %s", read("work/local/sam.dta").c_str());
    return 0;
}
