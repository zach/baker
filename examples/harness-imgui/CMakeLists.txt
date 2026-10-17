# The reference harness: Dear ImGui panels over Baker, in a GLFW and OpenGL 3
# window. Dear ImGui and GLFW are fetched at pinned releases, never vendored, as EASTL is.
# The panels are also built into a headless test that needs no window: ImGui with no
# backend, a few frames, and assertions on what changed.
include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        f1cc2ae15e53a861a874c3034aae6798fde194ab)  # v1.92.9b
FetchContent_MakeAvailable(imgui)

# Dear ImGui has no CMake build of its own: its core sources, without the demo.
add_library(baker_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
target_include_directories(baker_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR})
target_compile_features(baker_imgui PUBLIC cxx_std_20)
# Its sources are not ours to warn about.
set_target_properties(baker_imgui PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
target_compile_options(baker_imgui PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)

add_library(baker_harness_panels STATIC panels.cpp)
target_link_libraries(baker_harness_panels PUBLIC baker::script baker::shape baker_imgui)

if(BAKER_BUILD_TESTS)
  add_executable(baker_harness_test test_panels.cpp)
  target_link_libraries(baker_harness_test PRIVATE baker_harness_panels baker_doctest_main)
  add_test(NAME baker_harness_test COMMAND baker_harness_test
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

# The window: GLFW and OpenGL 3.
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)  # X11 only on Linux: fewer build dependencies
FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        7b6aead9fb88b3623e3b3725ebb42670cbe4c579)  # 3.4
FetchContent_MakeAvailable(glfw)
set_target_properties(glfw PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
target_compile_options(glfw PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
find_package(OpenGL REQUIRED)

add_executable(baker_harness main.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(baker_harness SYSTEM PRIVATE ${imgui_SOURCE_DIR}/backends)
target_link_libraries(baker_harness PRIVATE baker_harness_panels glfw OpenGL::GL)
target_compile_definitions(baker_harness PRIVATE
  BAKER_HARNESS_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
