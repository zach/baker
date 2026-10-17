#pragma once

#include <string_view>

namespace baker::fuzz {

// Parses `data` with no filesystem and tight limits. If it parses, prints the tree and
// parses the printout again. Returns false (a bug) if a successful parse does not
// round-trip, or if an error comes back without a usable location.
bool check_parse(std::string_view data);

// The first line of `data` is a Path, the rest DTA text. Looks the path up in every typed
// form and checks that nothing crashes, every error is located, a path prints back as
// itself, and sorted-mode find agrees with linear find on every array in the tree.
bool check_lookup(std::string_view data);

}  // namespace baker::fuzz
