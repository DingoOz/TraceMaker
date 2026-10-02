# tm_set_warnings(<target>): project-wide warning policy for C++ and CUDA sources.
function(tm_set_warnings target)
  set(cxx_warnings -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor
                   -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual
                   -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
  if(TM_WERROR)
    list(APPEND cxx_warnings -Werror)
  endif()
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${cxx_warnings}>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall,-Wextra --expt-relaxed-constexpr>)
  if(TM_WERROR)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Werror=all-warnings>)
  endif()
endfunction()
