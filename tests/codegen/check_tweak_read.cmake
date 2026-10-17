# Spec §7: reading a Tweak<float> compiles to a single relaxed atomic load. Compiles
# tweak_read.cpp at -O2 to assembly and inspects read_box_width():
#   - no call or branch (no function call, no lock, no loop);
#   - with -fno-pic on x86-64, exactly one instruction besides endbr64 and ret;
#   - otherwise at most three (position-independent code may load the address first).
# Invoked by CTest with CXX, STD, SRC, INC, DEFS (;-list), FLAGS (;-list), OUT, ARCH.

function(read_body pic_flag out_var)
  execute_process(
    COMMAND ${CXX} -std=c++${STD} -O2 -S ${pic_flag} ${FLAGS} -I${INC} ${DEFS} ${SRC} -o ${OUT}
    RESULT_VARIABLE rc ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "compiling ${SRC} failed:\n${err}")
  endif()
  file(STRINGS ${OUT} lines)
  set(body "")
  set(inside FALSE)
  foreach(line IN LISTS lines)
    if(line MATCHES "^_?_Z14read_box_widthv:")
      set(inside TRUE)
    elseif(inside AND (line MATCHES "^[^ \t.#]" OR line MATCHES "\\.cfi_endproc|^\t\\.size"))
      break()
    elseif(inside AND line MATCHES "^[ \t]+[a-z]")
      string(STRIP "${line}" insn)
      if(NOT insn MATCHES "^(endbr64|bti|ret|retq|\\.)")
        list(APPEND body "${insn}")
      endif()
    endif()
  endforeach()
  set(${out_var} "${body}" PARENT_SCOPE)
endfunction()

function(check body max what)
  list(LENGTH body n)
  string(REPLACE ";" "\n  " shown "${body}")
  if(n EQUAL 0)
    message(FATAL_ERROR "read_box_width() not found in the assembly (${what})")
  endif()
  foreach(insn IN LISTS body)
    if(insn MATCHES "^(call|jmp|j[a-z]+|bl|b|b\\.[a-z]+|cbn?z|lock)[ \t]")
      message(FATAL_ERROR "a Tweak<float> read branches or calls (${what}):\n  ${shown}")
    endif()
  endforeach()
  if(n GREATER max)
    message(FATAL_ERROR "a Tweak<float> read is ${n} instructions, expected at most ${max} (${what}):\n  ${shown}")
  endif()
  message(STATUS "${what}: ${shown}")
endfunction()

read_body("" body)
check("${body}" 3 "default code model")
if(ARCH MATCHES "x86_64|AMD64|amd64")
  read_body("-fno-pic" body)
  check("${body}" 1 "-fno-pic")
endif()
