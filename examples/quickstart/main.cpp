// The five-minute path: a constant becomes a tweak, read from default.dta and reloaded.
#include <baker/config.hpp>

#include <cstdio>
#include <iostream>
#include <string>

// Before: const float kPlayerSpeed = 4.0f;
static baker::Tweak<float> kPlayerSpeed{"player.speed", 4.0f};

int main() {
    baker::Config cfg;  // binds every Tweak in the program
    if (auto r = cfg.push_layer("default", "default.dta"); !r) {
        std::printf("%s\n", baker::to_string(r.error()).data());
        return 1;
    }
    std::printf("player speed %g\n", double(kPlayerSpeed));

    // Edit default.dta, save, and press Enter. A game calls reload() once a frame instead.
    for (std::string line; std::getline(std::cin, line);) {
        if (auto r = cfg.reload(); !r) std::printf("%s\n", baker::to_string(r.error()).data());
        std::printf("player speed %g\n", double(kPlayerSpeed));
    }
    return 0;
}
