# This file sets up a CMakeCache for a toolchain that builds programs for
# NT-POSIX: the Windows Itanium toolchain, with NT-POSIX's targets and LLVM libc
# as their C library. From the llvm-project directory:
#
#   cmake -G Ninja -C clang/cmake/caches/NTPOSIX-runtimes.cmake llvm
#
# It takes the Windows Itanium runtimes cache's place with
# WindowsItanium-toolchain.cmake and WindowsItanium-distribution.cmake.

set(LLVM_ENABLE_RUNTIMES "libc;compiler-rt;libunwind;libcxxabi;libcxx"
    CACHE STRING "")
set(LLVM_RUNTIME_TARGETS "x86_64-pc-windows-ntposix" CACHE STRING "")

foreach(target IN LISTS LLVM_RUNTIME_TARGETS)
  # The runtimes build LLVM libc, which the other runtimes link. Their
  # configuration checks cannot link before it exists.
  set(RUNTIMES_${target}_LLVM_LIBC_FULL_BUILD ON CACHE BOOL "")
  set(RUNTIMES_${target}_LIBC_ENABLE_USE_BY_CLANG ON CACHE BOOL "")
  set(RUNTIMES_${target}_RUNTIMES_USE_LIBC llvm-libc CACHE STRING "")
  set(RUNTIMES_${target}_CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY
      CACHE STRING "")
endforeach()

include(${CMAKE_CURRENT_LIST_DIR}/WindowsItanium-runtimes.cmake)
