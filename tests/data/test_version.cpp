#include <doctest.h>
#include <baker/std_interop.hpp>
#include <baker/version.hpp>

#include <string>

TEST_CASE("version string matches version constants") {
    const std::string expected = std::to_string(baker::kVersionMajor) + "." +
                                 std::to_string(baker::kVersionMinor) + "." +
                                 std::to_string(baker::kVersionPatch);
    CHECK(baker::version() == expected);
}
