# A game compiled with BAKER_SHIPPING other than the library's must fail to link, naming the
# configuration, and one compiled the same must link. Invoked by CTest with CXX, STD,
# SRC, INC, LIB (the baker_data archive), SHIPPING (the library's: 0 or 1), DEFS (;-list),
# FLAGS (;-list: the build's compile and link flags), OUT.
function(try_link shipping out_var log_var)
  execute_process(
    COMMAND ${CXX} -std=c++${STD} ${FLAGS} ${DEFS} -DBAKER_SHIPPING=${shipping} -I${INC} ${SRC} ${LIB}
            -lpthread -o ${OUT}
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(${out_var} ${rc} PARENT_SCOPE)
  set(${log_var} "${out}${err}" PARENT_SCOPE)
endfunction()

if(SHIPPING)
  set(other 0)
else()
  set(other 1)
endif()
try_link(${SHIPPING} same_rc same_log)
if(NOT same_rc EQUAL 0)
  message(FATAL_ERROR "a game built as the library was did not link:\n${same_log}")
endif()
try_link(${other} other_rc other_log)
if(other_rc EQUAL 0)
  message(FATAL_ERROR "a game built with BAKER_SHIPPING=${other} linked against a library built with ${SHIPPING}")
endif()
if(NOT other_log MATCHES "ConfiguredFor")
  message(FATAL_ERROR "the mismatch failed, but not on the configuration:\n${other_log}")
endif()
message(STATUS "BAKER_SHIPPING=${other} against ${SHIPPING}: refused at link time")
