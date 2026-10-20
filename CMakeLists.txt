cmake_minimum_required(VERSION 3.20)
project(baker VERSION 0.2.0 LANGUAGES CXX)

option(BAKER_BUILD_SCRIPT     "Build baker::script (evaluation, objects, tasks)" ON)
option(BAKER_BUILD_SHAPE      "Build baker::shape (data-declared shapes)" ON)
option(BAKER_BUILD_HOST       "Build baker::host (the host kit: watcher, console, live loop, commands)" ON)
option(BAKER_BUILD_TESTS      "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(BAKER_ATOMIC_REFCOUNT  "Atomic refcounts (OFF: plain counters, single-threaded use only)" ON)
option(BAKER_SHIPPING         "Freeze config and compile out dev instrumentation" OFF)
option(BAKER_BUILD_TOOLS      "Build command-line tools (baker_parse_corpus, baker_bake)" ${PROJECT_IS_TOP_LEVEL})
option(BAKER_BUILD_BENCH      "Build benchmarks" OFF)
option(BAKER_BUILD_EXAMPLES   "Build the examples" ${PROJECT_IS_TOP_LEVEL})
option(BAKER_BUILD_HARNESS    "Build examples/harness-imgui (fetches Dear ImGui and GLFW)" OFF)
option(BAKER_BUILD_FUZZERS    "Build libFuzzer targets (Clang only)" OFF)
option(BAKER_USE_EASTL        "Internal containers from EASTL instead of the STL" OFF)
set(BAKER_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined")

if(NOT DEFINED CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings and sanitizers for everything this project builds (not for consumers).
if(MSVC)
  # /Zc:preprocessor: the conforming preprocessor. The traditional one mangles raw string
  # literals inside macro arguments, such as doctest's CHECK(...).
  add_compile_options(/W4 /permissive- /utf-8 /Zc:preprocessor)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)  # fopen is fine; we check every result
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()
if(BAKER_BUILD_FUZZERS)
  # Coverage instrumentation for everything the fuzzer exercises, not just its main.
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()
# Baker never throws, and everything here builds with exceptions disabled.
if(MSVC)
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  add_compile_options(/EHs-c-)
  add_compile_definitions(_HAS_EXCEPTIONS=0)
else()
  add_compile_options(-fno-exceptions)
endif()

# Whether the library's Expected<T> is std::expected, decided once at configure time and
# exported, so code built at a different standard cannot silently disagree.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles([[
#include <version>
#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
#error no std::expected
#endif
int main() { return 0; }
]] BAKER_HAVE_STD_EXPECTED)
if(BAKER_HAVE_STD_EXPECTED)
  set(BAKER_STD_EXPECTED_VALUE 1)
else()
  set(BAKER_STD_EXPECTED_VALUE 0)
endif()
if(BAKER_SANITIZE)
  # -fno-sanitize-recover: undefined behavior aborts, so tests and the fuzzer fail on it.
  add_compile_options(-fsanitize=${BAKER_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${BAKER_SANITIZE})
endif()

# EASTL for src/'s containers (src/common/containers.hpp). A host that builds EASTL itself
# provides the EASTL target first; otherwise it is fetched at a pinned commit, never vendored.
if(BAKER_USE_EASTL AND NOT TARGET EASTL)
  include(FetchContent)
  FetchContent_Declare(EASTL
    GIT_REPOSITORY https://github.com/electronicarts/EASTL.git
    GIT_TAG        e323f67e5d93fdadcd44f7998069d804697b1945)  # 3.27.01
  FetchContent_MakeAvailable(EASTL)
  # Its headers are not ours to warn about, and its sources are not ours to fix.
  foreach(t EASTL EABase)
    if(TARGET ${t})
      get_target_property(dirs ${t} INTERFACE_INCLUDE_DIRECTORIES)
      set_target_properties(${t} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dirs}")
    endif()
  endforeach()
  set_target_properties(EASTL PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
  target_compile_options(EASTL PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
endif()

function(baker_configure_library target)
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
  if(BAKER_USE_EASTL)
    target_compile_definitions(${target} PRIVATE BAKER_USE_EASTL=1)
    target_link_libraries(${target} PRIVATE EASTL)
  endif()
  # Always explicit: mixing atomic and plain refcounts across translation units is an ODR
  # violation, so consumers must see the library's choice.
  if(BAKER_ATOMIC_REFCOUNT)
    target_compile_definitions(${target} PUBLIC BAKER_ATOMIC_REFCOUNT=1)
  else()
    target_compile_definitions(${target} PUBLIC BAKER_ATOMIC_REFCOUNT=0)
  endif()
  target_compile_definitions(${target} PUBLIC BAKER_STD_EXPECTED=${BAKER_STD_EXPECTED_VALUE})
  foreach(flag BAKER_SHIPPING)
    if(${flag})
      target_compile_definitions(${target} PUBLIC ${flag}=1)
    endif()
  endforeach()
endfunction()

# --- baker::data: nodes, arrays, parsing, lookup, config, tweaks, binary cache.
#     Must never depend on baker::script.
add_library(baker_data
  src/data/allocator.cpp
  src/data/arena.cpp
  src/data/clock.cpp
  src/data/array.cpp
  src/data/cache.cpp
  src/data/config.cpp
  src/data/config_loop.cpp
  src/data/config_write.cpp
  src/data/config_check.cpp
  src/data/project.cpp
  src/data/error.cpp
  src/data/lookup.cpp
  src/data/merge.cpp
  src/data/node.cpp
  src/data/parse.cpp
  src/data/repeats.cpp
  src/data/source_map.cpp
  src/data/symbol.cpp
  src/data/text.cpp
  src/data/tweak.cpp
  src/data/version.cpp
  src/data/write.cpp)
add_library(baker::data ALIAS baker_data)
baker_configure_library(baker_data)
# fast_float parses doubles identically on every platform (libc++ only gained double
# std::from_chars in LLVM 20). Private: it never appears in Baker's headers.
target_include_directories(baker_data SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/fast_float)

# --- baker::script: evaluation, functions, objects, messages, tasks.
if(BAKER_BUILD_SCRIPT)
  add_library(baker_script
    src/script/builtins.cpp
    src/script/config_tools.cpp
    src/script/context.cpp
    src/script/object.cpp
    src/script/reference.cpp
    src/script/request.cpp
    src/script/script_tasks.cpp
    src/script/task.cpp)
  add_library(baker::script ALIAS baker_script)
  baker_configure_library(baker_script)
  target_link_libraries(baker_script PUBLIC baker::data)
endif()

# --- baker::shape: shapes declared in data, checked at load. Depends only
#     on baker::data.
if(BAKER_BUILD_SHAPE)
  add_library(baker_shape src/shape/shape.cpp)
  add_library(baker::shape ALIAS baker_shape)
  baker_configure_library(baker_shape)
  target_link_libraries(baker_shape PUBLIC baker::data)
endif()

# --- baker::host: the host kit, what every host would write: the files the stack was
#     read from, watched; the loopback console; the live loop; the tool commands. Beside the core, not in it: it touches the operating system. Needs
#     baker::script; baker::baker does not link it.
if(BAKER_BUILD_HOST AND BAKER_BUILD_SCRIPT)
  add_library(baker_host src/host/commands.cpp src/host/console.cpp src/host/live.cpp
              src/host/watcher.cpp)
  add_library(baker::host ALIAS baker_host)
  baker_configure_library(baker_host)
  target_link_libraries(baker_host PUBLIC baker::script)
  if(BAKER_BUILD_SHAPE)  # Live adds the shape validator when there is one
    target_link_libraries(baker_host PUBLIC baker::shape)
    target_compile_definitions(baker_host PRIVATE BAKER_HOST_SHAPE=1)
  endif()
  if(WIN32)
    target_link_libraries(baker_host PRIVATE ws2_32)
  endif()
endif()

# --- baker::baker: umbrella.
add_library(baker INTERFACE)
add_library(baker::baker ALIAS baker)
target_link_libraries(baker INTERFACE baker::data)
if(BAKER_BUILD_SCRIPT)
  target_link_libraries(baker INTERFACE baker::script)
endif()
if(BAKER_BUILD_SHAPE)
  target_link_libraries(baker INTERFACE baker::shape)
endif()

if(BAKER_BUILD_TESTS OR BAKER_BUILD_BENCH)
  # The allocation counter (it replaces operator new), compiled once for the tests and the
  # benchmarks.
  add_library(baker_alloc_counter OBJECT tests/support/alloc_counter.cpp)
  target_include_directories(baker_alloc_counter PUBLIC tests/support)
  target_link_libraries(baker_alloc_counter PUBLIC baker::data)
endif()

if(BAKER_BUILD_TESTS OR BAKER_BUILD_FUZZERS)
  # Shared by the corpus tests and the fuzzers.
  add_library(baker_parse_harness STATIC fuzz/parse_harness.cpp fuzz/lookup_harness.cpp)
  target_include_directories(baker_parse_harness PUBLIC fuzz)
  target_link_libraries(baker_parse_harness PUBLIC baker::data)
endif()

if(BAKER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(BAKER_BUILD_TOOLS)
  add_executable(baker_parse_corpus tools/parse_corpus.cpp)
  target_link_libraries(baker_parse_corpus PRIVATE baker::data)
  add_executable(baker_bake tools/baker_bake.cpp)
  target_link_libraries(baker_bake PRIVATE baker::data)
  if(BAKER_BUILD_TESTS)
    add_test(NAME baker_bake COMMAND baker_bake -D DEBUG
      ${PROJECT_SOURCE_DIR}/tests/corpus/config_app.dta ${CMAKE_CURRENT_BINARY_DIR}/config_app.datb)
  endif()
endif()

if(BAKER_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(BAKER_BUILD_BENCH)
  add_executable(baker_bench_parse bench/bench_parse.cpp)
  target_link_libraries(baker_bench_parse PRIVATE baker::data baker_alloc_counter)
  add_executable(baker_bench_cache bench/bench_cache.cpp)
  target_link_libraries(baker_bench_cache PRIVATE baker::data baker_alloc_counter)
  add_executable(baker_bench_lookup bench/bench_lookup.cpp)
  target_link_libraries(baker_bench_lookup PRIVATE baker::data)
  add_executable(baker_bench_layers bench/bench_layers.cpp)
  target_link_libraries(baker_bench_layers PRIVATE baker::data)
  if(BAKER_BUILD_SHAPE)  # the same, with shape::validator judging each candidate
    add_executable(baker_bench_layers_shape bench/bench_layers.cpp)
    target_compile_definitions(baker_bench_layers_shape PRIVATE BAKER_BENCH_SHAPE)
    target_link_libraries(baker_bench_layers_shape PRIVATE baker::shape)
  endif()
  if(BAKER_BUILD_SCRIPT)
    add_executable(baker_bench_tasks bench/bench_tasks.cpp)
    target_link_libraries(baker_bench_tasks PRIVATE baker::script baker_alloc_counter)
    # The 128 bound types build as four units, in parallel.
    set(baker_bench_call_parts)
    foreach(part RANGE 3)
      set(unit ${CMAKE_CURRENT_BINARY_DIR}/bench_call_part${part}.cpp)
      file(CONFIGURE OUTPUT ${unit} CONTENT
           "#include \"bench_call_wide.hpp\"\ntemplate void push_part<${part}>(int, Objects&);\n")
      list(APPEND baker_bench_call_parts ${unit})
    endforeach()
    add_executable(baker_bench_call bench/bench_call.cpp ${baker_bench_call_parts})
    target_include_directories(baker_bench_call PRIVATE bench)
    target_link_libraries(baker_bench_call PRIVATE baker::script baker_alloc_counter)
  endif()
endif()

if(BAKER_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "BAKER_BUILD_FUZZERS requires Clang (libFuzzer)")
  endif()
  foreach(check parse lookup)
    add_executable(baker_${check}_fuzzer fuzz/libfuzzer_main.cpp)
    target_link_libraries(baker_${check}_fuzzer PRIVATE baker_parse_harness)
    target_compile_definitions(baker_${check}_fuzzer PRIVATE BAKER_FUZZ_CHECK=check_${check})
    target_compile_options(baker_${check}_fuzzer PRIVATE -fsanitize=fuzzer)
    target_link_options(baker_${check}_fuzzer PRIVATE -fsanitize=fuzzer)
  endforeach()
endif()
