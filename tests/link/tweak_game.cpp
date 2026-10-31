// Linked by check_config_mismatch.cmake: a game with one tweak, compiled with BAKER_SHIPPING
// as given, against the library as built.
#include <baker/tweak.hpp>

static baker::Tweak<float> kWidth{"box.width", 2.0f};

int main() {
    return kWidth > 0 ? 0 : 1;
}
