// libFuzzer entry point, shared by the fuzzers; BAKER_FUZZ_CHECK names the harness.
// Build with -DBAKER_BUILD_FUZZERS=ON (Clang), then e.g.
//   build/baker_parse_fuzzer -dict=fuzz/dta.dict tests/corpus
#include "parse_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (!baker::fuzz::BAKER_FUZZ_CHECK(text)) std::abort();
    return 0;
}
