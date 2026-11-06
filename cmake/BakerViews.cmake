# baker_generate_views(<target> PROJECT <baker-project.dta> [OUTPUT <dir>])
#
# Generates the project's C++ views (docs/language.md, Views) with `baker_bake --views` into
# <dir> (default: baker_views/<target> under the current binary directory), adds it to
# <target>'s include path, and regenerates whenever a file of the contract stack or the
# project file changes: the tool writes a depfile, and rewrites a header only when its text
# changed, so a tuning edit rebuilds nothing. Generated headers are build output.
#
# A cross-compile sets BAKER_BAKE_EXECUTABLE to a baker_bake built for the host.

set(BAKER_BAKE_EXECUTABLE "" CACHE FILEPATH
    "baker_bake built for the host, for a cross-compile; empty: this build's own")

function(baker_generate_views target)
  cmake_parse_arguments(ARG "" "PROJECT;OUTPUT" "" ${ARGN})
  if(NOT ARG_PROJECT)
    message(FATAL_ERROR "baker_generate_views(${target}): PROJECT <baker-project.dta> is required")
  endif()
  get_filename_component(project "${ARG_PROJECT}" ABSOLUTE)
  if(NOT ARG_OUTPUT)
    set(ARG_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/baker_views/${target})
  endif()
  if(BAKER_BAKE_EXECUTABLE)
    set(tool ${BAKER_BAKE_EXECUTABLE})
    set(tool_dep)
  else()
    set(tool $<TARGET_FILE:baker_bake>)
    set(tool_dep baker_bake)
  endif()
  set(stamp ${ARG_OUTPUT}/views.stamp)
  set(depfile ${ARG_OUTPUT}/views.d)
  add_custom_command(
    OUTPUT ${stamp}
    COMMAND ${tool} --views ${ARG_OUTPUT} --project ${project} --depfile ${depfile}
            --stamp ${stamp}
    DEPENDS ${project} ${tool_dep}
    DEPFILE ${depfile}
    COMMENT "Generating Baker views for ${target}"
    VERBATIM)
  add_custom_target(${target}_baker_views DEPENDS ${stamp})
  add_dependencies(${target} ${target}_baker_views)
  target_include_directories(${target} PRIVATE ${ARG_OUTPUT})
endfunction()
