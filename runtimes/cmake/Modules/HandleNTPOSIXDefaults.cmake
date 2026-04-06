# HandleNTPOSIXDefaults.cmake - NTPOSIX runtime build defaults
#
# NTPOSIX: POSIX-on-NT using llvm-libc as the sole C runtime.
# No UCRT, no Win32 thread API, always compiler-rt.

include_guard(GLOBAL)

include(DetectWindowsItanium)

option(RUNTIMES_NTPOSIX_DEFAULTS
  "Apply recommended configuration for NTPOSIX runtimes"
  ${WIN32_NTPOSIX})

if(NOT RUNTIMES_NTPOSIX_DEFAULTS)
  return()
endif()

message(STATUS "Applying NTPOSIX defaults (RUNTIMES_NTPOSIX_DEFAULTS=ON)")

# Long-path-enabled Windows installations can handle object paths well beyond
# CMake's conservative default. Raise the threshold to avoid premature hashing
# and spurious warnings for the deeply nested NT POSIX runtime tree.
set(CMAKE_OBJECT_PATH_MAX 32768 CACHE STRING "" FORCE)

# Pre-seed compiler flag checks that would otherwise fail.
# check_cxx_compiler_flag / llvm_check_compiler_linker_flag try to link an
# executable, but NTPOSIX CRT startup objects (crt1.obj) aren't available
# during configure — they're part of the runtimes being built.  Clang always
# supports these flags; only the link step is missing.
set(CXX_SUPPORTS_NOSTDLIBXX_FLAG ON CACHE INTERNAL
  "Forced ON for NTPOSIX — link check cannot succeed during bootstrap")
set(CXX_SUPPORTS_NOSTDINCXX_FLAG ON CACHE INTERNAL
  "Forced ON for NTPOSIX — link check cannot succeed during bootstrap")
set(CXX_SUPPORTS_NOSTDLIBINC_FLAG ON CACHE INTERNAL
  "Forced ON for NTPOSIX — link check cannot succeed during bootstrap")
set(CXX_SUPPORTS_UNWINDLIB_EQ_NONE_FLAG ON CACHE INTERNAL
  "Forced ON for NTPOSIX — link check cannot succeed during bootstrap")

# Set cache variable only if not already defined.
function(set_ntposix_default var value type docstring)
  if(NOT DEFINED ${var})
    set(${var} ${value} CACHE ${type} "${docstring}")
  endif()
endfunction()

#===------------------------------------------------------------------------===#
# Linker Configuration
#===------------------------------------------------------------------------===#

# LLD required for auto-import; MS link.exe lacks support.
if(NOT DEFINED LLVM_ENABLE_LLD AND NOT DEFINED LLVM_USE_LINKER)
  set(LLVM_ENABLE_LLD ON CACHE BOOL "Use LLD linker")
endif()

#===------------------------------------------------------------------------===#
# Bootstrap Link Flags
#===------------------------------------------------------------------------===#

# During the runtimes build the NTPOSIX toolchain driver injects c.lib
# (llvm-libc) and NT kernel import libraries as default libraries, but c.lib
# doesn't exist yet — it is part of what is being built.  Pass -nolibc so the
# driver keeps compiler-rt builtins and the unwinder but skips c.lib and the
# kernel import libraries.  Each shared runtime that needs NT kernel libraries
# adds them explicitly.
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -nolibc"
  CACHE STRING "" FORCE)

#===------------------------------------------------------------------------===#
# libunwind Configuration
#===------------------------------------------------------------------------===#

set_ntposix_default(LIBUNWIND_ENABLE_SHARED ON BOOL
  "Build libunwind as shared library")
set_ntposix_default(LIBUNWIND_ENABLE_STATIC OFF BOOL
  "Build libunwind as static library")
set_ntposix_default(LIBUNWIND_USE_COMPILER_RT ON BOOL
  "Use compiler-rt instead of libgcc")

#===------------------------------------------------------------------------===#
# libc++abi Configuration
#===------------------------------------------------------------------------===#

# Static libc++abi linked into libc++ DLL to break circular dependency.
set_ntposix_default(LIBCXXABI_ENABLE_SHARED OFF BOOL
  "Build libc++abi as shared library")
set_ntposix_default(LIBCXXABI_ENABLE_STATIC ON BOOL
  "Build libc++abi as static library")
set_ntposix_default(LIBCXXABI_ENABLE_THREADS ON BOOL
  "Build with threads enabled")
# NTPOSIX always uses pthread API via llvm-libc.
set_ntposix_default(LIBCXXABI_HAS_PTHREAD_API ON BOOL
  "Use pthread API (provided by llvm-libc)")
set_ntposix_default(LIBCXXABI_USE_COMPILER_RT ON BOOL
  "Use compiler-rt")

#===------------------------------------------------------------------------===#
# libc++ Configuration
#===------------------------------------------------------------------------===#

set_ntposix_default(LIBCXX_ENABLE_SHARED ON BOOL
  "Build libc++ as shared library")
set_ntposix_default(LIBCXX_ENABLE_STATIC OFF BOOL
  "Build libc++ as static library")
set_ntposix_default(LIBCXX_ABI_FORCE_ITANIUM ON BOOL
  "Force Itanium ABI")
# NTPOSIX always uses pthread API via llvm-libc.
set_ntposix_default(LIBCXX_HAS_PTHREAD_API ON BOOL
  "Use pthread API (provided by llvm-libc)")
set_ntposix_default(LIBCXX_ENABLE_WIDE_CHARACTERS ON BOOL
  "Enable wide characters (llvm-libc provides wide I/O)")
set_ntposix_default(LIBCXX_CXX_ABI "libcxxabi" STRING
  "C++ ABI library")
set_ntposix_default(LIBCXX_ENABLE_STATIC_ABI_LIBRARY ON BOOL
  "Use static ABI library")
set_ntposix_default(LIBCXX_NO_VCRUNTIME ON BOOL
  "No VC runtime dependency")
set_ntposix_default(LIBCXX_USE_COMPILER_RT ON BOOL
  "Use compiler-rt")
set_ntposix_default(LIBCXX_INSTALL_MODULES ON BOOL
  "Install C++ module sources for import std")

#===------------------------------------------------------------------------===#
# libc Configuration
#===------------------------------------------------------------------------===#

# llvm-libc is the sole C runtime for NTPOSIX.
set_ntposix_default(LLVM_LIBC_FULL_BUILD ON BOOL
  "Full llvm-libc build (not overlay mode)")

# NTPOSIX uses #pragma section / __declspec(allocate) for CRT and fork-reinit
# init sections. These are Microsoft extensions that Clang supports with
# -fms-extensions.
if(NOT CMAKE_C_FLAGS MATCHES "-fms-extensions")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fms-extensions" CACHE STRING "" FORCE)
endif()
if(NOT CMAKE_CXX_FLAGS MATCHES "-fms-extensions")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fms-extensions" CACHE STRING "" FORCE)
endif()
set_ntposix_default(LIBC_ENABLE_SHARED ON BOOL
  "Build LLVM libc as a shared library (c.dll)")
set_ntposix_default(LIBC_ENABLE_STATIC OFF BOOL
  "Build LLVM libc as a static library")

# Place c.lib (import library for c.dll) in the top-level build lib directory
# and libc headers in the target-specific include directory so the just-built
# clang can find them via its default search paths. Without this, libc outputs
# end up in deep runtimes subdirectories and the stage-2 bootstrap cannot
# locate them.
set_ntposix_default(LIBC_ENABLE_USE_BY_CLANG ON BOOL
  "Place libc output where the just-built clang can find it")

#===------------------------------------------------------------------------===#
# compiler-rt Configuration
#===------------------------------------------------------------------------===#

# wincrt is the UCRT-to-Itanium bridge — NTPOSIX uses llvm-libc startup instead.
set_ntposix_default(COMPILER_RT_BUILD_WINCRT OFF BOOL
  "NTPOSIX does not use wincrt (llvm-libc provides startup)")

# Scudo standalone as libc allocator — disabled until scudo/libc integration
# works on Windows.
set_ntposix_default(LLVM_LIBC_INCLUDE_SCUDO OFF BOOL
  "Use scudo standalone as the allocator for LLVM libc")
if(LLVM_LIBC_INCLUDE_SCUDO)
  set_ntposix_default(COMPILER_RT_BUILD_SANITIZERS ON BOOL
    "Enable compiler-rt sanitizer build (for scudo standalone)")
  set_ntposix_default(COMPILER_RT_SANITIZERS_TO_BUILD "scudo_standalone" STRING
    "Only build scudo standalone allocator")
  set_ntposix_default(COMPILER_RT_BUILD_SCUDO_STANDALONE_WITH_LLVM_LIBC ON BOOL
    "Build scudo with LLVM libc headers")
else()
  set_ntposix_default(COMPILER_RT_BUILD_SANITIZERS OFF BOOL
    "Sanitizers disabled (scudo not in use)")
endif()
set_ntposix_default(COMPILER_RT_BUILD_XRAY OFF BOOL
  "Unused compiler-rt runtime component")
set_ntposix_default(COMPILER_RT_BUILD_LIBFUZZER OFF BOOL
  "Unused compiler-rt runtime component")
set_ntposix_default(COMPILER_RT_BUILD_PROFILE OFF BOOL
  "Unused compiler-rt runtime component")
set_ntposix_default(COMPILER_RT_BUILD_CTX_PROFILE OFF BOOL
  "Unused compiler-rt runtime component")
set_ntposix_default(COMPILER_RT_BUILD_MEMPROF OFF BOOL
  "Unused compiler-rt runtime component")
set_ntposix_default(COMPILER_RT_BUILD_ORC OFF BOOL
  "Unused compiler-rt runtime component")
set_ntposix_default(COMPILER_RT_BUILD_GWP_ASAN OFF BOOL
  "Unused compiler-rt runtime component")

# NTPOSIX uses c++.lib naming (no 'lib' prefix) to match Clang's
# -lc++ expectations.
set(CMAKE_STATIC_LIBRARY_PREFIX "" CACHE STRING "No lib prefix on Windows")

#===------------------------------------------------------------------------===#
# Configuration Validation
#===------------------------------------------------------------------------===#

if(LIBCXXABI_ENABLE_SHARED AND LIBCXX_ENABLE_SHARED)
  message(WARNING
    "NTPOSIX: Building both libc++abi and libc++ as shared libraries "
    "may cause circular dependency issues. The recommended configuration is "
    "LIBCXXABI_ENABLE_SHARED=OFF with LIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON.")
endif()

set(using_lld OFF)
if(LLVM_ENABLE_LLD)
  set(using_lld ON)
elseif(LLVM_USE_LINKER MATCHES "^lld")
  set(using_lld ON)
elseif(CMAKE_LINKER MATCHES "(^|/)(ld\\.)?lld(-link)?(\\.exe)?$")
  set(using_lld ON)
endif()

if(NOT using_lld)
  message(WARNING
    "NTPOSIX: LLD is required for auto-import support. "
    "Set LLVM_ENABLE_LLD=ON or LLVM_USE_LINKER=lld. "
    "Current settings: LLVM_ENABLE_LLD=${LLVM_ENABLE_LLD}, "
    "LLVM_USE_LINKER=${LLVM_USE_LINKER}, CMAKE_LINKER=${CMAKE_LINKER}")
endif()
