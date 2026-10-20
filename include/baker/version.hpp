#pragma once

#include <string_view>

namespace baker {

inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 1;
inline constexpr int kVersionPatch = 1;

// Version string of the baker::data library this program linked against.
[[nodiscard]] std::string_view version() noexcept;

}  // namespace baker
