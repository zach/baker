# Third-party code

Everything vendored into this repository, with its license. Only MIT, BSD, or
Apache-2.0 code may be added.

| Component | Version | License | Location | Used by |
|-----------|---------|---------|----------|---------|
| [doctest](https://github.com/doctest/doctest) | 2.4.11 | MIT (`third_party/doctest/LICENSE.txt`) | `third_party/doctest/` | Tests only |
| [fast_float](https://github.com/fastfloat/fast_float) | 8.3.2 | MIT, chosen from its MIT / Apache-2.0 / BSL-1.0 options (`third_party/fast_float/LICENSE-MIT`) | `third_party/fast_float/` | `baker::data` (private; parses floating-point literals) |

Fetched, not vendored: with `BAKER_USE_EASTL=ON` only, CMake fetches these at a pinned
commit for `src/`'s containers. The default build uses neither.

| Component | Version | License | Used by |
|-----------|---------|---------|---------|
| [EASTL](https://github.com/electronicarts/EASTL) | 3.27.01 (`e323f67`) | BSD-3-Clause | `baker::data`, `baker::script` (private) |
| [EABase](https://github.com/electronicarts/EABase) | as EASTL pins it | BSD-3-Clause | EASTL |
