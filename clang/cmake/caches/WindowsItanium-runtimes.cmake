# This file sets up a CMakeCache for a toolchain that builds programs for
# Windows Itanium: clang and lld, and for each of LLVM_RUNTIME_TARGETS,
# compiler-rt's builtins and start-up library, libunwind, libc++abi and libc++.
# From the llvm-project directory:
#
#   cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -C clang/cmake/caches/WindowsItanium-runtimes.cmake llvm
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

# A toolchain built on Windows with the MSVC CRT allocates through rpmalloc,
# as the release builds do, which is faster than the CRT's heap. rpmalloc takes
# the static CRT, and supports neither Debug builds nor the sanitizers, so this
# needs the build type before this file on the command line; CMake's default
# for an MSVC-style compiler is Debug. A toolchain built for Windows Itanium, as
# the second stage of WindowsItanium-toolchain.cmake is, keeps the UCRT's
# allocator, which its runtimes share.
string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" AND
   NOT CMAKE_TOOLCHAIN_FILE STREQUAL toolchain_file AND
   build_type MATCHES "^(RELEASE|RELWITHDEBINFO|MINSIZEREL)$" AND
   NOT LLVM_USE_SANITIZER)
  set(LLVM_ENABLE_RPMALLOC ON CACHE BOOL "")
endif()

# For clang-cl, CMake takes LLVM's archiver, linker and manifest tool, but its
# MASM rules still name Visual C++'s ml64, so a Windows host without it
# assembles with llvm-ml64, preferring the one next to the compiler.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" AND
   NOT CMAKE_TOOLCHAIN_FILE STREQUAL toolchain_file AND
   NOT CMAKE_ASM_MASM_COMPILER)
  find_program(ml64_compiler NAMES ml64)
  if(NOT ml64_compiler)
    set(ml_hints)
    if(CMAKE_C_COMPILER)
      get_filename_component(ml_hints "${CMAKE_C_COMPILER}" DIRECTORY)
    endif()
    find_program(llvm_ml64_compiler NAMES llvm-ml64 HINTS ${ml_hints}
                 NO_DEFAULT_PATH)
    if(NOT llvm_ml64_compiler)
      find_program(llvm_ml64_compiler NAMES llvm-ml64)
    endif()
    if(llvm_ml64_compiler)
      set(CMAKE_ASM_MASM_COMPILER "${llvm_ml64_compiler}" CACHE FILEPATH "")
    endif()
    unset(llvm_ml64_compiler CACHE)
  endif()
  unset(ml64_compiler CACHE)
endif()

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
  # No sanitizer runtime is built for these targets. CFI, which traps, reads
  # upstream's ignorelist.
  set(RUNTIMES_${target}_COMPILER_RT_BUILD_SANITIZERS OFF CACHE BOOL "")
  set(RUNTIMES_${target}_COMPILER_RT_BUILD_SANITIZER_IGNORELISTS ON CACHE BOOL "")
  set(RUNTIMES_${target}_COMPILER_RT_BUILD_LIBFUZZER OFF CACHE BOOL "")
  # ORC's COFF platform bootstraps the JIT'd program with the MSVC CRT, which
  # these targets do not have.
  set(RUNTIMES_${target}_COMPILER_RT_BUILD_ORC OFF CACHE BOOL "")

  # libc++abi is linked into both libc++.dll and libc++.lib. Every image links
  # compiler-rt's builtins.
  set(RUNTIMES_${target}_LIBCXXABI_ENABLE_SHARED OFF CACHE BOOL "")
  set(RUNTIMES_${target}_LIBCXX_ENABLE_STATIC_ABI_LIBRARY ON CACHE BOOL "")
  set(RUNTIMES_${target}_LIBCXX_USE_COMPILER_RT ON CACHE BOOL "")
  set(RUNTIMES_${target}_LIBCXXABI_USE_COMPILER_RT ON CACHE BOOL "")
  set(RUNTIMES_${target}_LIBUNWIND_USE_COMPILER_RT ON CACHE BOOL "")
  # Programs trap on the checks of libc++'s fast hardening mode unless they
  # define _LIBCPP_HARDENING_MODE themselves.
  set(RUNTIMES_${target}_LIBCXX_HARDENING_MODE fast CACHE STRING "")
  # A failed check fails fast, which no exception handler can resume past.
  set(RUNTIMES_${target}_LIBCXX_ASSERTION_HANDLER_FILE
      vendor/windows-itanium/assertion_handler.in CACHE FILEPATH "")
endforeach()
