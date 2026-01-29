# CMakeLists.txt for compiler-rt CRT startup libraries
#
# This builds minimal CRT startup objects for Windows targets that don't use
# msvcrt.lib or vcruntime.lib, such as Windows Itanium. Each entry point
# variant is built as a separate object file that can be linked by the driver.
#
# Features provided:
#   - CRT entry points (mainCRTStartup, wmainCRTStartup, etc.)
#   - Thread-safe atexit() implementation with __cxa_atexit integration
#   - exit() with proper C++ destructor ordering via __cxa_finalize
#   - Command line parsing (argc/argv) and environment (envp)
#   - Security cookie support (/GS stack protection)
#   - Thread Local Storage (TLS) support for __declspec(thread)
#   - Standard CRT globals (__argc, __argv, _environ, etc.)
#
# The objects are installed as:
#   clang_rt.crt_main-<arch>.obj     - for main() entry point
#   clang_rt.crt_wmain-<arch>.obj    - for wmain() entry point
#   clang_rt.crt_winmain-<arch>.obj  - for WinMain() entry point
#   clang_rt.crt_wwinmain-<arch>.obj - for wWinMain() entry point
#   clang_rt.crt_dllmain-<arch>.obj  - for DllMain() entry point

include(AddCompilerRT)

# Only build for Windows targets
if(NOT WIN32)
  message(STATUS "CRT: Skipping (not Windows target)")
  return()
endif()

# Require Clang compiler (these objects use Clang-specific attributes)
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(STATUS "CRT: Skipping (requires Clang compiler)")
  return()
endif()

# Create the crt component (may already exist from builtins if both are built)
if(NOT TARGET crt)
  add_compiler_rt_component(crt)
endif()

set(CRT_WINDOWS_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/crt_windows.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/crt_windows_cmdline.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/crt_windows_init.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/crt_windows_pseudo_reloc.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/crt_windows_security.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/crt_windows_tls.cpp
)

# Common compile flags - freestanding environment with no standard library
# C++ CONSTRAINTS: This code runs before the C++ runtime is initialized.
# - NO exceptions (throw, try/catch)
# - NO RTTI (dynamic_cast, typeid)
# - NO heap (new/delete) - we ARE the memory init code
# - NO static constructors - we ARE the constructor runner
# - NO std:: anything
set(CRT_CFLAGS
  -ffreestanding       # No hosted environment assumptions
  -fno-builtin         # Don't replace calls with builtins
  -fno-exceptions      # No C++ exceptions
  -fno-rtti            # No RTTI (dynamic_cast, typeid)
  -fms-extensions      # Enable MS-specific extensions for Windows types
  -O2                  # Optimize for size/speed balance
  -Wno-incompatible-library-redeclaration  # Suppress warnings about our imports
)

# Don't add default runtime library references (like MSVC's /Zl)
if(CMAKE_C_SIMULATE_ID STREQUAL "MSVC" OR MSVC)
  list(APPEND CRT_CFLAGS /Zl)
endif()

# Define the CRT object variants and their defines
# Each produces a separate .obj file with a specific entry point
set(CRT_VARIANTS
  main      # mainCRTStartup for int main()
  wmain     # wmainCRTStartup for int wmain()
  winmain   # WinMainCRTStartup for WinMain()
  wwinmain  # wWinMainCRTStartup for wWinMain()
  dllmain   # _DllMainCRTStartup for DllMain()
)

# Determine which architectures we can target
# Use BUILTIN_SUPPORTED_ARCH if available (from builtins), otherwise detect
if(DEFINED BUILTIN_SUPPORTED_ARCH)
  set(CRT_SUPPORTED_ARCH ${BUILTIN_SUPPORTED_ARCH})
else()
  # Fallback: detect based on target
  set(CRT_SUPPORTED_ARCH)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|X64")
      list(APPEND CRT_SUPPORTED_ARCH x86_64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
      list(APPEND CRT_SUPPORTED_ARCH aarch64)
    endif()
  else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|i[3-6]86|X86")
      list(APPEND CRT_SUPPORTED_ARCH i386)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM")
      list(APPEND CRT_SUPPORTED_ARCH arm)
    endif()
  endif()
endif()

# Filter to only Windows-capable architectures
set(CRT_WINDOWS_ARCH)
foreach(arch ${CRT_SUPPORTED_ARCH})
  if(arch MATCHES "x86_64|i386|i686|aarch64|arm")
    list(APPEND CRT_WINDOWS_ARCH ${arch})
  endif()
endforeach()

if(NOT CRT_WINDOWS_ARCH)
  message(STATUS "CRT: No supported Windows architectures found")
  return()
endif()

message(STATUS "CRT: Building Windows CRT objects for architectures: ${CRT_WINDOWS_ARCH}")

# Build each variant for each supported architecture
foreach(variant ${CRT_VARIANTS})
  string(TOUPPER ${variant} VARIANT_UPPER)

  # Use add_compiler_rt_runtime which handles per-architecture builds properly
  add_compiler_rt_runtime(clang_rt.crt_${variant}
    OBJECT
    ARCHS ${CRT_WINDOWS_ARCH}
    SOURCES ${CRT_WINDOWS_SOURCES}
    CFLAGS ${CRT_CFLAGS}
    DEFS CRT_HAS_${VARIANT_UPPER}
    PARENT_TARGET crt
  )
endforeach()

# Ensure crt is built as part of compiler-rt
add_dependencies(compiler-rt crt)
