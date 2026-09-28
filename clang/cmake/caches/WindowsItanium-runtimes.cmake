# This file sets up a CMakeCache for a toolchain that builds programs for
# Windows Itanium: clang and lld, and for each of LLVM_RUNTIME_TARGETS,
# compiler-rt's builtins and start-up library, libunwind, libc++abi and libc++.
# From the llvm-project directory:
#
#   cmake -G Ninja -C clang/cmake/caches/WindowsItanium-runtimes.cmake llvm
#
# The runtimes are built with the clang being built, on any host. A host other
# than Windows must give that clang the Windows SDK, for example in a
# configuration file beside it.

set(LLVM_ENABLE_PROJECTS "clang;lld" CACHE STRING "")
set(LLVM_ENABLE_RUNTIMES "compiler-rt;libunwind;libcxxabi;libcxx"
    CACHE STRING "")
set(LLVM_TARGETS_TO_BUILD "AArch64;X86" CACHE STRING "")
set(LLVM_ENABLE_PER_TARGET_RUNTIME_DIR ON CACHE BOOL "")

# The toolchain targets the first of these by default.
set(LLVM_RUNTIME_TARGETS "x86_64-unknown-windows-itanium" CACHE STRING "")
set(LLVM_BUILTIN_TARGETS "${LLVM_RUNTIME_TARGETS}" CACHE STRING "")
list(GET LLVM_RUNTIME_TARGETS 0 default_target)
set(LLVM_DEFAULT_TARGET_TRIPLE "${default_target}" CACHE STRING "")

cmake_path(APPEND CMAKE_CURRENT_LIST_DIR ../../../llvm/cmake/platforms
           WindowsItaniumToolchain.cmake OUTPUT_VARIABLE toolchain_file)

foreach(target IN LISTS LLVM_RUNTIME_TARGETS)
  # CMake would take the GNU-style driver for MinGW's.
  set(BUILTINS_${target}_CMAKE_TOOLCHAIN_FILE ${toolchain_file}
      CACHE FILEPATH "")
  set(RUNTIMES_${target}_CMAKE_TOOLCHAIN_FILE ${toolchain_file}
      CACHE FILEPATH "")
  # On a Windows host, the runtimes are not cross-compiled and their tests run.
  if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(BUILTINS_${target}_CMAKE_SYSTEM_NAME Windows CACHE STRING "")
    set(RUNTIMES_${target}_CMAKE_SYSTEM_NAME Windows CACHE STRING "")
  endif()

  # The builtins build makes Windows Itanium's start-up library; this enables
  # its tests.
  if(target MATCHES "-windows-itanium")
    set(RUNTIMES_${target}_COMPILER_RT_BUILD_CRT ON CACHE BOOL "")
  endif()
  # The driver supports no sanitizer for these targets.
  set(RUNTIMES_${target}_COMPILER_RT_BUILD_SANITIZERS OFF CACHE BOOL "")
  set(RUNTIMES_${target}_COMPILER_RT_BUILD_LIBFUZZER OFF CACHE BOOL "")

  # libc++abi is linked into both libc++.dll and libc++.lib. Every image links
  # compiler-rt's builtins.
  set(RUNTIMES_${target}_LIBCXXABI_ENABLE_SHARED OFF CACHE BOOL "")
  set(RUNTIMES_${target}_LIBCXX_ENABLE_STATIC_ABI_LIBRARY ON CACHE BOOL "")
  set(RUNTIMES_${target}_LIBCXX_USE_COMPILER_RT ON CACHE BOOL "")
  set(RUNTIMES_${target}_LIBCXXABI_USE_COMPILER_RT ON CACHE BOOL "")
  set(RUNTIMES_${target}_LIBUNWIND_USE_COMPILER_RT ON CACHE BOOL "")
endforeach()
