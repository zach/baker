# doctest's implementation, once for every test binary; baker_doctest_main adds a
# main to it.
add_library(baker_doctest STATIC doctest_impl.cpp)
target_include_directories(baker_doctest SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_compile_features(baker_doctest PUBLIC cxx_std_20)
# doctest 2.4.11 forward-declares std::basic_ostream, which newer MSVC STLs (14.44+)
# reject; include the real standard headers instead.
target_compile_definitions(baker_doctest PUBLIC DOCTEST_CONFIG_USE_STD_HEADERS)
# Built without exceptions: REQUIRE terminates instead of throwing.
target_compile_definitions(baker_doctest PUBLIC DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
add_library(baker_doctest_main STATIC doctest_main.cpp)
target_link_libraries(baker_doctest_main PUBLIC baker_doctest)

add_executable(baker_data_tests
  data/test_arena.cpp
  data/test_cache.cpp
  data/test_config.cpp
  data/test_core.cpp
  data/test_corpus.cpp
  data/test_lookup.cpp
  data/test_m3b.cpp
  data/test_m10.cpp
  data/test_explain.cpp
  data/test_tweaks_json.cpp
  data/test_layer_check.cpp
  data/test_received_layers.cpp
  data/test_layer_checker.cpp
  data/test_project.cpp
  data/test_parse.cpp
  data/test_preprocessor.cpp
  data/test_version.cpp
  data/test_write.cpp
  readiness_loop.cpp)
target_link_libraries(baker_data_tests PRIVATE baker::data baker_parse_harness baker_doctest_main
                      baker_alloc_counter)
# White-box tests of internal helpers (text.hpp).
target_include_directories(baker_data_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/data support)
if(BAKER_USE_EASTL)  # the white-box tests include src/data/text.hpp
  target_compile_definitions(baker_data_tests PRIVATE BAKER_USE_EASTL=1)
  target_link_libraries(baker_data_tests PRIVATE EASTL)
endif()
target_compile_definitions(baker_data_tests PRIVATE
  BAKER_CORPUS_DIR="${PROJECT_SOURCE_DIR}/tests/corpus"
  BAKER_FUZZ_REGRESSIONS_DIR="${PROJECT_SOURCE_DIR}/tests/fuzz_regressions")
add_test(NAME baker_data_tests COMMAND baker_data_tests)

if(BAKER_BUILD_SCRIPT)
  add_executable(baker_script_tests
    script/test_binding.cpp
    script/test_check.cpp
    script/test_debug.cpp
    script/test_eval.cpp
    script/test_messages.cpp
    script/test_object.cpp
    script/test_script_tasks.cpp
    script/test_stats.cpp
    script/test_tasks.cpp
    script/test_request.cpp
    script/test_config_tools.cpp
    script/test_reference_json.cpp)
  target_link_libraries(baker_script_tests PRIVATE baker::script baker_doctest_main
                        baker_alloc_counter)
  target_compile_definitions(baker_script_tests PRIVATE
    BAKER_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    BAKER_CORPUS_DIR="${PROJECT_SOURCE_DIR}/tests/corpus")
  add_test(NAME baker_script_tests COMMAND baker_script_tests)
endif()

if(BAKER_BUILD_SHAPE)
  add_executable(baker_shape_tests shape/test_shape.cpp)
  target_link_libraries(baker_shape_tests PRIVATE baker::shape baker_doctest_main
                        baker_alloc_counter)
  target_compile_definitions(baker_shape_tests PRIVATE BAKER_SHAPE_DIR="${PROJECT_SOURCE_DIR}/tests/shape")
  add_test(NAME baker_shape_tests COMMAND baker_shape_tests)
endif()

if(BAKER_BUILD_SCRIPT)
  # Its own binary, because main installs the default allocator before anything runs.
  add_executable(baker_alloc_tests alloc/main.cpp alloc/test_allocators.cpp)
  target_link_libraries(baker_alloc_tests PRIVATE baker::script baker_doctest baker_alloc_counter)
  add_test(NAME baker_alloc_tests COMMAND baker_alloc_tests)
endif()

# Spec §7: a Tweak<float> read is a single load. Checked in the generated assembly with
# GCC and Clang, unless sanitizers or fuzzing instrument every load.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT BAKER_SANITIZE AND NOT BAKER_BUILD_FUZZERS)
  separate_arguments(baker_codegen_flags UNIX_COMMAND "${CMAKE_CXX_FLAGS}")
  add_test(NAME baker_tweak_codegen COMMAND ${CMAKE_COMMAND}
    -DCXX=${CMAKE_CXX_COMPILER}
    -DSTD=${CMAKE_CXX_STANDARD}
    -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/codegen/tweak_read.cpp
    -DINC=${PROJECT_SOURCE_DIR}/include
    "-DDEFS=-DBAKER_STD_EXPECTED=${BAKER_STD_EXPECTED_VALUE};-DBAKER_ATOMIC_REFCOUNT=$<BOOL:${BAKER_ATOMIC_REFCOUNT}>"
    "-DFLAGS=${baker_codegen_flags}"
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/tweak_read.s
    -DARCH=${CMAKE_SYSTEM_PROCESSOR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/codegen/check_tweak_read.cmake)
endif()

# Public headers reach only the allowed standard headers (std_*.hpp excepted), and each
# compiles on its own.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME baker_header_allowlist
           COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/check_headers.py)
endif()
file(GLOB baker_public_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/include/baker/*.hpp)
set(baker_header_units)
foreach(header ${baker_public_headers})
  get_filename_component(name ${header} NAME_WE)
  set(unit ${CMAKE_CURRENT_BINARY_DIR}/header_alone/${name}.cpp)
  # CONFIGURE, not WRITE: an unchanged unit keeps its time stamp and is not recompiled.
  file(CONFIGURE OUTPUT ${unit} CONTENT "#include <baker/${name}.hpp>\n")
  list(APPEND baker_header_units ${unit})
endforeach()
add_library(baker_header_alone OBJECT ${baker_header_units})
if(BAKER_BUILD_SCRIPT)
  target_link_libraries(baker_header_alone PRIVATE baker::script)
else()
  target_link_libraries(baker_header_alone PRIVATE baker::data)
endif()
