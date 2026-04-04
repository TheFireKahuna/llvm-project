# DetectMSVCLike.cmake - Detect MSVC-like build environments
#
# Provides:
#   MSVC_LIKE - TRUE for MSVC, clang-cl, Windows Itanium, or NTPOSIX targets
#
# Use this for conditionals that apply to all MSVC-compatible environments,
# such as skipping Unix-specific library checks.

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
