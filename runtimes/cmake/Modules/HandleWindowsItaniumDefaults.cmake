# HandleWindowsItaniumDefaults.cmake - Windows Itanium ABI build defaults
#
# Provides recommended defaults for building runtimes targeting Windows with
# the Itanium C++ ABI (libc++/libc++abi/libunwind).
# See: https://llvm.org/docs/HowToBuildWindowsItaniumPrograms.html
#
# The option RUNTIMES_WINDOWS_ITANIUM_DEFAULTS controls whether to apply the
# recommended build configuration. It auto-detects based on the target triple
# but can be explicitly controlled:
#   cmake -DRUNTIMES_WINDOWS_ITANIUM_DEFAULTS=ON ...   # Force defaults on
#   cmake -DRUNTIMES_WINDOWS_ITANIUM_DEFAULTS=OFF ...  # Disable defaults
#
# When defaults are enabled, they can still be overridden individually:
#   cmake -DLIBUNWIND_ENABLE_STATIC=ON -DRUNTIMES_WINDOWS_ITANIUM_DEFAULTS=ON ...

include_guard(GLOBAL)

# Auto-detect Windows Itanium target from triple.
set(_is_windows_itanium OFF)
if(CMAKE_C_COMPILER_TARGET MATCHES "windows-itanium" OR
   CMAKE_CXX_COMPILER_TARGET MATCHES "windows-itanium" OR
   LLVM_DEFAULT_TARGET_TRIPLE MATCHES "windows-itanium")
  set(_is_windows_itanium ON)
endif()

option(RUNTIMES_WINDOWS_ITANIUM_DEFAULTS
  "Apply recommended configuration for Windows Itanium runtimes"
  ${_is_windows_itanium})

if(NOT RUNTIMES_WINDOWS_ITANIUM_DEFAULTS)
  return()
endif()

message(STATUS "Applying Windows Itanium defaults (RUNTIMES_WINDOWS_ITANIUM_DEFAULTS=ON)")

# The WindowsItaniumToolChain driver automatically handles:
# - SJLJ exception model (the only currently supported model)
# - Required defines (_LIBCPP_ABI_FORCE_ITANIUM, _NO_CRT_STDIO_INLINE)
# - Default libraries (msvcrt, ucrt, legacy_stdio_definitions, etc.)
# - Auto-import for vtable pseudo-relocations (-auto-import)
# - LLD linker selection

# Helper: set cache variable only if not already defined, allowing user overrides.
function(set_windows_itanium_default var value type docstring)
  if(NOT DEFINED ${var})
    set(${var} ${value} CACHE ${type} "${docstring}")
  endif()
endfunction()

#===------------------------------------------------------------------------===#
# Bootstrapping Configuration
#===------------------------------------------------------------------------===#

# Windows Itanium requires runtime libraries (libunwind, libc++, libc++abi) that
# are built by LLVM itself, not provided by the system. CMake's try_compile tests
# would fail when trying to link because these libraries may not exist yet.
#
# Use STATIC_LIBRARY mode to compile without linking. This is safe because the
# flag checks (e.g., -funwind-tables, -fno-exceptions) only need compilation to
# succeed, not linking.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY CACHE STRING
  "Compile-only for try_compile (Windows Itanium runtimes may not exist yet)")

#===------------------------------------------------------------------------===#
# Linker Configuration
#===------------------------------------------------------------------------===#

# Use LLD linker (required for auto-import support; MS link.exe cannot be used).
# LLVM supports two mutually exclusive ways to select LLD:
#   - LLVM_ENABLE_LLD=ON (preferred, specific to LLD)
#   - LLVM_USE_LINKER=lld (general linker selection)
# We use LLVM_ENABLE_LLD, but only if neither option is already set.
if(NOT DEFINED LLVM_ENABLE_LLD AND NOT DEFINED LLVM_USE_LINKER)
  set(LLVM_ENABLE_LLD ON CACHE BOOL "Use LLD linker for Windows Itanium")
endif()

#===------------------------------------------------------------------------===#
# libunwind Configuration
#===------------------------------------------------------------------------===#

# Build as shared DLL (recommended for Windows Itanium).
set_windows_itanium_default(LIBUNWIND_ENABLE_SHARED ON BOOL
  "Build libunwind as a shared library.")
set_windows_itanium_default(LIBUNWIND_ENABLE_STATIC OFF BOOL
  "Build libunwind as a static library.")
set_windows_itanium_default(LIBUNWIND_USE_COMPILER_RT OFF BOOL
  "Use compiler-rt instead of libgcc")

#===------------------------------------------------------------------------===#
# libc++abi Configuration
#===------------------------------------------------------------------------===#

# Build as static library (will be linked into libc++ DLL).
# This is required to break the circular dependency between libc++ and libc++abi:
# exception_ptr in libc++ headers declares functions defined in libc++abi, but
# libc++abi includes those headers. Static linking resolves this at link time.
set_windows_itanium_default(LIBCXXABI_ENABLE_SHARED OFF BOOL
  "Build libc++abi as a shared library.")
set_windows_itanium_default(LIBCXXABI_ENABLE_STATIC ON BOOL
  "Build libc++abi as a static library.")
set_windows_itanium_default(LIBCXXABI_ENABLE_THREADS ON BOOL
  "Build with threads enabled")
set_windows_itanium_default(LIBCXXABI_HAS_WIN32_THREAD_API ON BOOL
  "Use win32 thread API")
set_windows_itanium_default(LIBCXXABI_USE_COMPILER_RT OFF BOOL
  "Use compiler-rt")

#===------------------------------------------------------------------------===#
# libc++ Configuration
#===------------------------------------------------------------------------===#

# Build as shared DLL with static libc++abi linked in.
set_windows_itanium_default(LIBCXX_ENABLE_SHARED ON BOOL
  "Build libc++ as a shared library.")
set_windows_itanium_default(LIBCXX_ENABLE_STATIC OFF BOOL
  "Build libc++ as a static library.")
set_windows_itanium_default(LIBCXX_ABI_FORCE_ITANIUM ON BOOL
  "Force use of Itanium ABI.")
set_windows_itanium_default(LIBCXX_HAS_WIN32_THREAD_API ON BOOL
  "Use win32 thread API")
set_windows_itanium_default(LIBCXX_CXX_ABI "libcxxabi" STRING
  "C++ ABI library to use.")
set_windows_itanium_default(LIBCXX_ENABLE_STATIC_ABI_LIBRARY ON BOOL
  "Use static ABI library.")
set_windows_itanium_default(LIBCXX_NO_VCRUNTIME ON BOOL
  "Remove dependency on VC runtime.")
set_windows_itanium_default(LIBCXX_USE_COMPILER_RT OFF BOOL
  "Use compiler-rt")

#===------------------------------------------------------------------------===#
# Configuration Validation
#===------------------------------------------------------------------------===#

# Validate configuration: warn about known-problematic settings.
# These checks run after defaults are applied, so they catch user overrides.

if(LIBCXXABI_ENABLE_SHARED AND LIBCXX_ENABLE_SHARED)
  message(WARNING
    "Windows Itanium: Building both libc++abi and libc++ as shared libraries "
    "may cause circular dependency issues. The recommended configuration is "
    "LIBCXXABI_ENABLE_SHARED=OFF with LIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON.")
endif()

# Check if LLD is being used. LLD can be selected via:
#   - LLVM_ENABLE_LLD=ON (sets up LLD)
#   - LLVM_USE_LINKER=lld (passed as -fuse-ld=lld)
#   - CMAKE_LINKER pointing to an lld binary (e.g., /path/to/ld.lld)
set(using_lld OFF)
if(LLVM_ENABLE_LLD)
  set(using_lld ON)
elseif(LLVM_USE_LINKER MATCHES "^lld")
  set(using_lld ON)
elseif(CMAKE_LINKER MATCHES "(^|/)(ld\\.)?lld(-link)?(\\.exe)?$")
  # Match lld, ld.lld, lld-link, and their .exe variants at the end of a path.
  # Avoids false positives like "/usr/bin/gold" matching "lld" substring.
  set(using_lld ON)
endif()

if(NOT using_lld)
  message(WARNING
    "Windows Itanium: LLD is required for auto-import support. "
    "Set LLVM_ENABLE_LLD=ON or LLVM_USE_LINKER=lld. "
    "Current settings: LLVM_ENABLE_LLD=${LLVM_ENABLE_LLD}, "
    "LLVM_USE_LINKER=${LLVM_USE_LINKER}, CMAKE_LINKER=${CMAKE_LINKER}")
endif()
