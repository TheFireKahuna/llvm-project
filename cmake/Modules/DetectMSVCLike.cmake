# DetectMSVCLike.cmake - Detect MSVC-like build environments
#
# Provides:
#   MSVC_LIKE - TRUE for MSVC, clang-cl, Windows Itanium, or NTPOSIX targets.
#               Use for conditionals that apply to all PE/COFF-targeting
#               environments, such as skipping Unix-specific library checks.
#
#   COMPILER_LINKER_IS_MSVC_STYLE - TRUE when the compiler driver accepts
#               MSVC-style linker flags (e.g. /opt:lldlto=0) directly.
#               FALSE when a GNU-style driver needs -Xlinker wrapping.
#               Use this (not MSVC_LIKE) for linker flag formatting decisions.
#
# Functions:
#   llvm_lld_link_flag(<out_var> <flag>)
#               Returns <flag> for MSVC-style drivers, or "-Xlinker <flag>"
#               for GNU-style drivers using lld-link.

include_guard(GLOBAL)

# MSVC-like: MSVC, clang-cl, Windows Itanium, or NTPOSIX (all target PE/COFF)
if(MSVC OR
   CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC" OR
   CMAKE_C_COMPILER_TARGET MATCHES "windows-(itanium|ntposix)" OR
   CMAKE_CXX_COMPILER_TARGET MATCHES "windows-(itanium|ntposix)" OR
   LLVM_RUNTIMES_TARGET MATCHES "windows-(itanium|ntposix)" OR
   LLVM_HOST_TRIPLE MATCHES "windows-(itanium|ntposix)")
  set(MSVC_LIKE TRUE)
else()
  set(MSVC_LIKE FALSE)
endif()

# MSVC and clang-cl invoke the linker directly and accept MSVC-style flags.
# GNU-style Clang (including Windows Itanium/NTPOSIX) needs -Xlinker wrapping.
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set(COMPILER_LINKER_IS_MSVC_STYLE TRUE)
else()
  set(COMPILER_LINKER_IS_MSVC_STYLE FALSE)
endif()

# llvm_lld_link_flag(<out_var> <flag>)
#   Format a single lld-link flag for the current compiler driver.
function(llvm_lld_link_flag out_var flag)
  if(COMPILER_LINKER_IS_MSVC_STYLE)
    set(${out_var} "${flag}" PARENT_SCOPE)
  else()
    set(${out_var} "-Xlinker ${flag}" PARENT_SCOPE)
  endif()
endfunction()
