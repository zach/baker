# A shipping build keeps none of a tweak's tool text, nor a function's or a global's doc:
# tweak_text.cpp at -O2 with
# BAKER_SHIPPING=1 has no TOOL_TEXT literal in its assembly, and without it has all five,
# so the test would see one. Invoked by CTest with CXX, STD, SRC, INC, DEFS (;-list),
# FLAGS (;-list), OUT.

function(count_text shipping out_var)
  execute_process(
    COMMAND ${CXX} -std=c++${STD} -O2 -S ${FLAGS} -I${INC} ${DEFS} -DBAKER_SHIPPING=${shipping}
            ${SRC} -o ${OUT}
    RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "compiling ${SRC} failed:\n${err}")
  endif()
  file(STRINGS ${OUT} lines REGEX "TOOL_TEXT_")
  list(LENGTH lines n)
  set(${out_var} ${n} PARENT_SCOPE)
endfunction()

count_text(0 dev)
if(NOT dev EQUAL 5)
  message(FATAL_ERROR "a development build has ${dev} of the 5 tool texts; expected all")
endif()
count_text(1 shipped)
if(NOT shipped EQUAL 0)
  message(FATAL_ERROR "a shipping build carries ${shipped} of a tweak's tool texts; expected none")
endif()
message(STATUS "tool texts: 5 in development, none shipped")
