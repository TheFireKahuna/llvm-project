# HandleWindowsItaniumDefaults.cmake - Windows Itanium runtime build defaults
#
# See: https://llvm.org/docs/HowToBuildWindowsItaniumPrograms.html

include_guard(GLOBAL)

include(DetectWindowsItanium)

option(RUNTIMES_WINDOWS_ITANIUM_DEFAULTS
  "Apply recommended configuration for Windows Itanium runtimes"
  ${WIN32_ITANIUM})

if(NOT RUNTIMES_WINDOWS_ITANIUM_DEFAULTS)
  return()
endif()

message(STATUS "Applying Windows Itanium defaults (RUNTIMES_WINDOWS_ITANIUM_DEFAULTS=ON)")

# Set cache variable only if not already defined.
function(set_windows_itanium_default var value type docstring)
  if(NOT DEFINED ${var})
    set(${var} ${value} CACHE ${type} "${docstring}")
  endif()
endfunction()

#===------------------------------------------------------------------------===#
# Compiler Definitions
#===------------------------------------------------------------------------===#

# Suppress MSVC CRT deprecation warnings. The driver provides
# _CRT_SECURE_NO_WARNINGS; add remaining suppression macros here.
add_compile_definitions(
  _CRT_NONSTDC_NO_WARNINGS
  _SCL_SECURE_NO_WARNINGS
)

#===------------------------------------------------------------------------===#
# Linker Configuration
#===------------------------------------------------------------------------===#

# LLD required for auto-import; MS link.exe lacks support.
if(NOT DEFINED LLVM_ENABLE_LLD AND NOT DEFINED LLVM_USE_LINKER)
  set(LLVM_ENABLE_LLD ON CACHE BOOL "Use LLD linker")
endif()

#===------------------------------------------------------------------------===#
# libunwind Configuration
#===------------------------------------------------------------------------===#

set_windows_itanium_default(LIBUNWIND_ENABLE_SHARED ON BOOL
  "Build libunwind as shared library")
set_windows_itanium_default(LIBUNWIND_ENABLE_STATIC OFF BOOL
  "Build libunwind as static library")
set_windows_itanium_default(LIBUNWIND_USE_COMPILER_RT OFF BOOL
  "Use compiler-rt instead of libgcc")

#===------------------------------------------------------------------------===#
# libc++abi Configuration
#===------------------------------------------------------------------------===#

# Static libc++abi linked into libc++ DLL to break circular dependency.
set_windows_itanium_default(LIBCXXABI_ENABLE_SHARED OFF BOOL
  "Build libc++abi as shared library")
set_windows_itanium_default(LIBCXXABI_ENABLE_STATIC ON BOOL
  "Build libc++abi as static library")
set_windows_itanium_default(LIBCXXABI_ENABLE_THREADS ON BOOL
  "Build with threads enabled")
if(LLVM_LIBC_FULL_BUILD)
  set_windows_itanium_default(LIBCXXABI_HAS_PTHREAD_API ON BOOL
    "Use pthread API (provided by llvm-libc)")
else()
  set_windows_itanium_default(LIBCXXABI_HAS_WIN32_THREAD_API ON BOOL
    "Use win32 thread API")
endif()
set_windows_itanium_default(LIBCXXABI_USE_COMPILER_RT OFF BOOL
  "Use compiler-rt")

#===------------------------------------------------------------------------===#
# libc++ Configuration
#===------------------------------------------------------------------------===#

set_windows_itanium_default(LIBCXX_ENABLE_SHARED ON BOOL
  "Build libc++ as shared library")
set_windows_itanium_default(LIBCXX_ENABLE_STATIC OFF BOOL
  "Build libc++ as static library")
set_windows_itanium_default(LIBCXX_ABI_FORCE_ITANIUM ON BOOL
  "Force Itanium ABI")
if(LLVM_LIBC_FULL_BUILD)
  set_windows_itanium_default(LIBCXX_HAS_PTHREAD_API ON BOOL
    "Use pthread API (provided by llvm-libc)")
  set_windows_itanium_default(LIBCXX_ENABLE_WIDE_CHARACTERS ON BOOL
    "Enable wide characters (llvm-libc provides wide I/O)")
else()
  set_windows_itanium_default(LIBCXX_HAS_WIN32_THREAD_API ON BOOL
    "Use win32 thread API")
endif()
set_windows_itanium_default(LIBCXX_CXX_ABI "libcxxabi" STRING
  "C++ ABI library")
set_windows_itanium_default(LIBCXX_ENABLE_STATIC_ABI_LIBRARY ON BOOL
  "Use static ABI library")
set_windows_itanium_default(LIBCXX_NO_VCRUNTIME ON BOOL
  "No VC runtime dependency")
set_windows_itanium_default(LIBCXX_USE_COMPILER_RT OFF BOOL
  "Use compiler-rt")
set_windows_itanium_default(LIBCXX_INSTALL_MODULES ON BOOL
  "Install C++ module sources for import std")
  
#===------------------------------------------------------------------------===#
# compiler-rt Configuration
#===------------------------------------------------------------------------===#
#
# builtins are needed when *_USE_COMPILER_RT is ON for any runtime.
# wincrt provides CRT entry points when libc is NOT providing startup.
#
# NOTE: To make --rtlib=compiler-rt the default for the driver, set
# CLANG_WIN32_ITANIUM_DEFAULT_RTLIB=compiler-rt in your cache file BEFORE
# clang is built (this file runs too late to affect the driver).


# Check if any runtime wants compiler-rt builtins.
set(_use_compiler_rt OFF)
if(LIBUNWIND_USE_COMPILER_RT OR LIBCXXABI_USE_COMPILER_RT OR LIBCXX_USE_COMPILER_RT)
  set(_use_compiler_rt ON)
endif()
#===------------------------------------------------------------------------===#
# libc Configuration
#===------------------------------------------------------------------------===#

# Shared c.dll is required — runtime DLLs (libunwind, libc++) must share a
# single libc instance to avoid duplicated global state (heap, fd table,
# signal handlers). Static c.lib is skipped to avoid import lib conflict.
set_windows_itanium_default(LIBC_ENABLE_SHARED ON BOOL
  "Build LLVM libc as a shared library (c.dll)")
set_windows_itanium_default(LIBC_ENABLE_STATIC OFF BOOL
  "Build LLVM libc as a static library")

#===------------------------------------------------------------------------===#
# compiler-rt Configuration
#===------------------------------------------------------------------------===#

# Scudo standalone as libc allocator — disabled until scudo/libc integration
# works on Windows. When re-enabled, set BUILD_SANITIZERS=ON and
# SANITIZERS_TO_BUILD=scudo_standalone.
set_windows_itanium_default(LLVM_LIBC_INCLUDE_SCUDO OFF BOOL
  "Use scudo standalone as the allocator for LLVM libc")
if(LLVM_LIBC_INCLUDE_SCUDO)
  set_windows_itanium_default(COMPILER_RT_BUILD_SANITIZERS ON BOOL
    "Enable compiler-rt sanitizer build (for scudo standalone)")
  set_windows_itanium_default(COMPILER_RT_SANITIZERS_TO_BUILD "scudo_standalone" STRING
    "Only build scudo standalone allocator")
  set_windows_itanium_default(COMPILER_RT_BUILD_SCUDO_STANDALONE_WITH_LLVM_LIBC ON BOOL
    "Build scudo with LLVM libc headers")
else()
  set_windows_itanium_default(COMPILER_RT_BUILD_SANITIZERS OFF BOOL
    "Sanitizers disabled (scudo not in use)")
endif()
set_windows_itanium_default(COMPILER_RT_BUILD_XRAY OFF BOOL
  "Unused compiler-rt runtime component")
set_windows_itanium_default(COMPILER_RT_BUILD_LIBFUZZER OFF BOOL
  "Unused compiler-rt runtime component")
set_windows_itanium_default(COMPILER_RT_BUILD_PROFILE OFF BOOL
  "Unused compiler-rt runtime component")
set_windows_itanium_default(COMPILER_RT_BUILD_CTX_PROFILE OFF BOOL
  "Unused compiler-rt runtime component")
set_windows_itanium_default(COMPILER_RT_BUILD_MEMPROF OFF BOOL
  "Unused compiler-rt runtime component")
set_windows_itanium_default(COMPILER_RT_BUILD_ORC OFF BOOL
  "Unused compiler-rt runtime component")
set_windows_itanium_default(COMPILER_RT_BUILD_GWP_ASAN OFF BOOL
  "Unused compiler-rt runtime component")

# Windows Itanium uses c++.lib naming (no 'lib' prefix) to match Clang's
# -lc++ expectations. Override the static library prefix set in libcxx/src.
set(CMAKE_STATIC_LIBRARY_PREFIX "" CACHE STRING "No lib prefix on Windows")

#===------------------------------------------------------------------------===#
# Configuration Validation
#===------------------------------------------------------------------------===#

if(LIBCXXABI_ENABLE_SHARED AND LIBCXX_ENABLE_SHARED)
  message(WARNING
    "Windows Itanium: Building both libc++abi and libc++ as shared libraries "
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
    "Windows Itanium: LLD is required for auto-import support. "
    "Set LLVM_ENABLE_LLD=ON or LLVM_USE_LINKER=lld. "
    "Current settings: LLVM_ENABLE_LLD=${LLVM_ENABLE_LLD}, "
    "LLVM_USE_LINKER=${LLVM_USE_LINKER}, CMAKE_LINKER=${CMAKE_LINKER}")
endif()
