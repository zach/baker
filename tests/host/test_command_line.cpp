// The kit's command line: its flags begin --baker-, and every other flag is the host's.
#include <baker/host/commands.hpp>

#include <doctest.h>

#include <string>
#include <vector>

namespace host = baker::host;

namespace {
baker::Expected<host::CommandLine> parse(std::vector<const char*> argv) {
    argv.insert(argv.begin(), "game");
    return host::parse_command_line(static_cast<int>(argv.size()), argv.data());
}
std::vector<std::string> rest(const host::CommandLine& line) {
    std::vector<std::string> out;
    for (const std::string_view a : line.rest) out.emplace_back(a);
    return out;
}
}  // namespace

TEST_CASE("the kit reads its --baker- flags and leaves the game's alone") {
    const auto line = parse({"sim", "--seed", "9", "--baker-seeds", "2-4", "--step-hz", "30",
                             "--baker-json", "--baker-project", "data", "{+ 1 2}", "--json"});
    REQUIRE(line);
    CHECK(line->command == "sim");
    CHECK(line->seed == 2);
    CHECK(line->seed_last == 4);
    CHECK(line->step_hz == 60);  // --step-hz is the game's, not the kit's
    CHECK(line->json);
    CHECK(line->project == "data");
    REQUIRE(line->commands.size() == 1);
    CHECK(line->commands[0] == "{+ 1 2}");
    CHECK(rest(*line) == std::vector<std::string>{"--seed", "9", "--step-hz", "30", "--json"});
}

TEST_CASE("a --baker- flag the kit does not know, or one missing its value, is an error") {
    const auto unknown = parse({"check", "--baker-sede", "3"});
    REQUIRE_FALSE(unknown);
    CHECK(std::string(unknown.error().message()) == "unknown flag --baker-sede");
    const auto missing = parse({"check", "--baker-output"});
    REQUIRE_FALSE(missing);
    CHECK(std::string(missing.error().message()) == "--baker-output needs a value");
    const auto bad = parse({"sim", "--baker-seeds", "4-2"});
    REQUIRE_FALSE(bad);
    CHECK(std::string(bad.error().message()).find("--baker-seeds") != std::string::npos);
}
