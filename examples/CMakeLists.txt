# Each example is a small program that CTest runs against its expected output (M9).

# A test that runs `target` with `args`, passing if its output matches `regex`. Examples
# run in a directory of their own under the build tree, so a run that writes files never
# touches the sources.
function(baker_example_test name target regex)
  set(dir ${CMAKE_CURRENT_BINARY_DIR}/run/${name})
  file(MAKE_DIRECTORY ${dir})
  add_test(NAME ${name} COMMAND ${target} ${ARGN} WORKING_DIRECTORY ${dir})
  set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

set(nl "\r?\n")
# Shipping builds make reload, live edits, and write-back inert, so examples that show
# them are tested only in development builds.
if(BAKER_BUILD_TESTS AND NOT BAKER_SHIPPING)
  set(baker_test_dev ON)
else()
  set(baker_test_dev OFF)
endif()

# --- quickstart: the README's five-minute path.
add_executable(baker_example_quickstart quickstart/main.cpp)
target_link_libraries(baker_example_quickstart PRIVATE baker::data)
add_executable(baker_example_quickstart_test quickstart/test_quickstart.cpp)
target_link_libraries(baker_example_quickstart_test PRIVATE baker::data)
if(baker_test_dev)
  baker_example_test(baker_example_quickstart baker_example_quickstart_test
    "^player speed 4[.]5${nl}player speed 6${nl}baker: [^\n]*default[.]dta:1[^\n]*${nl}player speed 6${nl}$"
    ${CMAKE_CURRENT_SOURCE_DIR}/quickstart/default.dta)
endif()
if(BAKER_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME baker_readme_blocks
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/check_readme.py)
  endif()
endif()

# --- one_line_tweak: the constant replacement, before and after.
add_executable(baker_example_one_line_tweak one_line_tweak/main.cpp)
target_link_libraries(baker_example_one_line_tweak PRIVATE baker::data)
if(BAKER_BUILD_TESTS)
  baker_example_test(baker_example_one_line_tweak baker_example_one_line_tweak
    "before loading: box width 2[^.0-9].*after loading:  box width 3[.]5 .*config[.]dta:3[)]  note: 3-4 reads well"
    ${CMAKE_CURRENT_SOURCE_DIR}/one_line_tweak/config.dta)
endif()

if(BAKER_BUILD_SCRIPT)
  # --- calculator: eval, with native and script functions.
  add_executable(baker_example_calculator calculator/main.cpp)
  target_link_libraries(baker_example_calculator PRIVATE baker::script)
  if(BAKER_BUILD_TESTS)
    baker_example_test(baker_example_calculator baker_example_calculator
      "^7${nl}49${nl}5\\.0${nl}3\\.0${nl}10${nl}$"
      --functions ${CMAKE_CURRENT_SOURCE_DIR}/calculator/functions.dta
      "{+ 1 {* 2 3}}" "{square 7}" "{hypot 3 4}" "{average $ans 1}" "{clamp 12 0 10}")
  endif()
endif()

# --- config: layers, provenance, reload callbacks, write-back.
add_executable(baker_example_config config/main.cpp)
target_link_libraries(baker_example_config PRIVATE baker::data)
if(baker_test_dev)
  baker_example_test(baker_example_config baker_example_config "shadows 0, volume 0[.]8.*renderer[.]shadows +layer handheld handheld[.]dta:3.*audio rebuilt: volume 0[.]6${nl}reload: .*audio[.]volume +layer scratch +[(]unsaved[)].*audio[.]volume +layer default +default[.]dta:6${nl}default[.]dta: +[(]volume 0[.]45 [(]range 0 1[)].*audio[.]channels +layer local +sam[.]dta:1${nl}local/sam[.]dta: [(]audio [(]channels 8[)][)]"
    ${CMAKE_CURRENT_SOURCE_DIR}/config/data)
endif()

# --- live-ops: published overlays staged, applied under a policy, refused, rolled back.
# Runs in shipping builds too: that is where it matters.
if(BAKER_BUILD_SHAPE)
  add_executable(baker_example_live_ops live-ops/main.cpp)
  target_link_libraries(baker_example_live_ops PRIVATE baker::shape)
  if(BAKER_BUILD_TESTS)
    baker_example_test(baker_example_live_ops baker_example_live_ops "xp x1, drops 0[.]05.*event[.]xp_multiplier: 1 -> 2.*event[.]banner: [(]none[)] -> .*xp x2, drops 0[.]05, banner \"Double XP all weekend!\"${nl}  event[.]xp_multiplier from layer overlays/2026-10-17-double-xp[.]dta [(]liveops console, ticket 4412, by baker-publish[)].*refused: baker: a warning refuses the layer: .drop_rate. is 15, outside 0[.]0 to 1[.]0  [(]overlays/2026-10-18-drops[.]dta:5[)].*requires .game_data. \"1[.]1\" or later, which is \"1[.]0\".*holds [{]random_float 0[.]05 0[.]5[}][;] the layer may hold data only.*roll back.*${nl}  xp x1, drops 0[.]05, banner \"\"${nl}layers: default scratch"
      ${CMAKE_CURRENT_SOURCE_DIR}/live-ops)
  endif()
endif()

if(BAKER_BUILD_SCRIPT)
  # --- scripting: natives, script functions, lookups that evaluate, eval, check.
  add_executable(baker_example_scripting scripting/main.cpp)
  target_link_libraries(baker_example_scripting PRIVATE baker::script)
  # --- objects: methods, properties, a typed message, {class}, a stale reference.
  add_executable(baker_example_objects objects/main.cpp)
  target_link_libraries(baker_example_objects PRIVATE baker::script)
  # --- tasks: a C++ coroutine and the script tasks.
  add_executable(baker_example_tasks tasks/main.cpp)
  target_link_libraries(baker_example_tasks PRIVATE baker::script)
  # --- remote-console: eval over TCP, tested over loopback, on the host kit's
  #     Console.
  if(TARGET baker::host)
    find_package(Threads REQUIRED)
    add_executable(baker_example_remote_console remote-console/main.cpp)
    target_link_libraries(baker_example_remote_console PRIVATE baker::host Threads::Threads)
    if(WIN32)
      target_link_libraries(baker_example_remote_console PRIVATE ws2_32)
    endif()
  endif()
  # --- host: the host kit, a game's tooling from its main.
  if(TARGET baker::host)
    add_executable(baker_example_host host/main.cpp)
    target_link_libraries(baker_example_host PRIVATE baker::host)
    if(BAKER_BUILD_TESTS)
      baker_example_test(baker_example_host_check baker_example_host "^{${nl}  \"schema\": \"baker[.]check/1[.]3\",${nl}  \"passed\": true"
        check --project ${CMAKE_CURRENT_SOURCE_DIR}/host)
      baker_example_test(baker_example_host_play baker_example_host
        "^player speed 4[.]5, 3 spawned${nl}$"
        play --frames 3 --console-port 0 --project ${CMAKE_CURRENT_SOURCE_DIR}/host)
    endif()
  endif()
  if(BAKER_BUILD_TESTS)
    baker_example_test(baker_example_scripting baker_example_scripting "takes 2 arguments, given 1  [(]rules[.]dta:11[)]${nl}demand: 'play_sfx' is not defined.*grunt hp 60, brute hp 160, brute damage 9${nl}at difficulty 3: brute hp 240, damage 12${nl}on_enter:${nl}  spawn 3 grunt${nl}  spawn 1 brute${nl}[{]scaled 10[}] => 30.*'%' by zero.*  in [{]ratio[}]"
      ${CMAKE_CURRENT_SOURCE_DIR}/scripting/rules.dta)
    baker_example_test(baker_example_objects baker_example_objects "dash 3[}] +=> 3[.]0${nl}.*=> 100${nl}grunt[.]handle[(]HitMsg[{]20[}][)] +=> 80${nl}.*hit 5[}] +=> 75${nl}.*=> Boss${nl}boss[.]handle[(]HitMsg[{]50[}][)] +=> 499${nl}.*hit 50[}] +=> 25${nl}.*=> \"Over here!\"${nl}.*was removed [(]a stale handle[)]")
    baker_example_test(baker_example_tasks baker_example_tasks "t=1  door: unlocking${nl}.*t=2  one unit later.*t=3  door: open.*t=3  guard: walking in.*t=4  fade done.*t=4  guard: closing the door${nl}t=5  door: someone closed it${nl}tasks left: 0")
    if(TARGET baker_example_remote_console AND NOT BAKER_SHIPPING)  # shipping: no console
      baker_example_test(baker_example_remote_console baker_example_remote_console "^> [{]set [$]speed [{][*] 2 4[.]5[}][}]${nl}=> 9[.]0${nl}${nl}> [{]boost 3[}]${nl}=> 27[.]0${nl}${nl}> [{]boost[}]${nl}baker: .boost. takes 1 argument, given 0  [(]<console>:1[)]${nl}  in [{]boost[}].*> [(]4 [{]boost 1[}][)]${nl}[{]\"schema\":\"baker[.]reply/1[.]1\",\"id\":4,\"ok\":true,\"text\":\"27[.]0\",\"value\":27[.]0[}]"
        --test)
    endif()
  endif()
endif()

# --- harness-imgui: the reference tuning harness. Off by default: it fetches Dear
# ImGui and GLFW, and its window needs OpenGL.
if(BAKER_BUILD_HARNESS)
  if(NOT BAKER_BUILD_SCRIPT OR NOT BAKER_BUILD_SHAPE)
    message(FATAL_ERROR "BAKER_BUILD_HARNESS needs BAKER_BUILD_SCRIPT and BAKER_BUILD_SHAPE")
  endif()
  add_subdirectory(harness-imgui)
endif()
