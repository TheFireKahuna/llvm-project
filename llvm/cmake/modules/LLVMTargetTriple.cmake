# Returns the triple that the code being configured is compiled for, and the
# runtime personality derived from it.
#
# CMake's WIN32 and UNIX do not carry the triple's environment, which is what
# distinguishes the Windows environments from each other. A runtimes build
# names its target through CMAKE_<LANG>_COMPILER_TARGET or
# LLVM_RUNTIMES_TARGET; an LLVM build names its host through
# LLVM_HOST_TRIPLE. LLVM_DEFAULT_TARGET_TRIPLE is not consulted: it is the
# default target of the compiler being built, not the triple of its host.

include_guard(GLOBAL)

function(llvm_get_effective_target_triple var)
  foreach(candidate IN ITEMS
      CMAKE_C_COMPILER_TARGET
      CMAKE_CXX_COMPILER_TARGET
      LLVM_RUNTIMES_TARGET
      LLVM_HOST_TRIPLE)
    if(${candidate})
      set(${var} "${${candidate}}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${var} "" PARENT_SCOPE)
endfunction()

# Sets LLVM_RUNTIME_WIN32 and LLVM_RUNTIME_POSIX, which name the C runtime and
# system interface that host code is written against, and
# LLVM_RUNTIME_NTPOSIX. They differ from WIN32 and LLVM_ON_UNIX only for
# NT-POSIX, a Windows target whose runtime is POSIX.
macro(llvm_set_runtime_personality)
  llvm_get_effective_target_triple(host_runtime_triple)
  set(LLVM_RUNTIME_WIN32 0)
  set(LLVM_RUNTIME_POSIX 0)
  set(LLVM_RUNTIME_NTPOSIX 0)
  if(WIN32)
    if(host_runtime_triple MATCHES "-windows-ntposix")
      set(LLVM_RUNTIME_POSIX 1)
      set(LLVM_RUNTIME_NTPOSIX 1)
    else()
      set(LLVM_RUNTIME_WIN32 1)
    endif()
  elseif(FUCHSIA OR UNIX OR CYGWIN)
    set(LLVM_RUNTIME_POSIX 1)
  endif()
  unset(host_runtime_triple)
endmacro()
