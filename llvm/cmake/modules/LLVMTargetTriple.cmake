# LLVMTargetTriple.cmake - Resolve the effective target triple
#
# During runtimes builds the per-target triple comes from
# CMAKE_C_COMPILER_TARGET, not LLVM_HOST_TRIPLE. This function checks
# multiple variables in priority order and returns the first non-empty value.

include_guard(GLOBAL)

# llvm_get_effective_target_triple(out_var)
#   Returns the first non-empty value from the standard triple variables.
function(llvm_get_effective_target_triple out_var)
  foreach(_var IN ITEMS
      CMAKE_C_COMPILER_TARGET
      CMAKE_CXX_COMPILER_TARGET
      LLVM_RUNTIMES_TARGET
      LLVM_TARGET_TRIPLE
      LLVM_DEFAULT_TARGET_TRIPLE
      LLVM_HOST_TRIPLE)
    if(${_var})
      set(${out_var} "${${_var}}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out_var} "" PARENT_SCOPE)
endfunction()
