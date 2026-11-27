# This file sets up a CMakeCache for a toolchain that builds programs for
# Windows Itanium: clang, lld and, for each of LLVM_RUNTIME_TARGETS,
# compiler-rt's builtins and start-up library, libunwind, libc++abi and libc++;
# and compiler-rt's builtins for NT-POSIX.
# From the llvm-project directory:
#
#   cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -C clang/cmake/caches/WindowsItanium-runtimes.cmake llvm

set(LLVM_ENABLE_PROJECTS "clang;lld" CACHE STRING "")
set(LLVM_ENABLE_RUNTIMES "compiler-rt;libunwind;libcxxabi;libcxx"
    CACHE STRING "")
set(LLVM_TARGETS_TO_BUILD "AArch64;X86" CACHE STRING "")
set(LLVM_RUNTIME_TARGETS
    "x86_64-unknown-windows-itanium;aarch64-unknown-windows-itanium"
    CACHE STRING "")
set(LLVM_BUILTIN_TARGETS ${LLVM_RUNTIME_TARGETS}
    x86_64-pc-windows-ntposix aarch64-pc-windows-ntposix CACHE STRING "")
# The driver finds a target's runtimes in its own directory.
set(LLVM_ENABLE_PER_TARGET_RUNTIME_DIR ON CACHE BOOL "")

# lld-link merges the manifest that the driver embeds in every executable
# itself only when built with libxml2.
set(LLVM_ENABLE_LIBXML2 FORCE_ON CACHE STRING "")

foreach(target IN LISTS LLVM_RUNTIME_TARGETS)
  # Of compiler-rt, these targets have the builtins and the start-up library,
  # which the builtins build makes; this build has their tests.
  set(RUNTIMES_${target}_COMPILER_RT_BUILD_CRT ON CACHE BOOL "")
  foreach(component SANITIZERS LIBFUZZER PROFILE ORC)
    set(RUNTIMES_${target}_COMPILER_RT_BUILD_${component} OFF CACHE BOOL "")
  endforeach()

  # libc++.dll and libc++.lib hold libc++abi.
  set(RUNTIMES_${target}_LIBCXXABI_ENABLE_SHARED OFF CACHE BOOL "")
  set(RUNTIMES_${target}_LIBCXX_ENABLE_STATIC_ABI_LIBRARY ON CACHE BOOL "")
  # Programs trap on the checks of libc++'s fast hardening mode unless they
  # define _LIBCPP_HARDENING_MODE themselves.
  set(RUNTIMES_${target}_LIBCXX_HARDENING_MODE fast CACHE STRING "")
endforeach()
