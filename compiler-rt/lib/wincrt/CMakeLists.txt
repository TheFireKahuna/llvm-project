# UCRT bridge for Windows Itanium - bridges UCRT/Win32 with Itanium C++ ABI.
#
# This library provides CRT startup code for Windows Itanium targets:
#   - Entry points (_DllMainCRTStartup, mainCRTStartup, etc.)
#   - CRT section processing (.CRT$X*)
#   - __cxa_atexit / __cxa_finalize implementation
#   - Security cookie (/GS) support
#   - PE load configuration
#   - Pseudo-relocation for COFF vtables
#
# IMPORTANT: This library must be built BEFORE the C++ runtimes (libunwind,
# libc++abi, libc++) since they need it to link as DLLs.

# Standalone build support - allows building wincrt before runtimes.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20.0)
  if(NOT DEFINED LLVM_COMMON_CMAKE_UTILS)
    set(LLVM_COMMON_CMAKE_UTILS ${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake)
  endif()

  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  project(CompilerRTWinCRT C CXX ASM)
  set(COMPILER_RT_STANDALONE_BUILD TRUE)
  set(COMPILER_RT_WINCRT_STANDALONE_BUILD TRUE)

  set(COMPILER_RT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../..")

  list(INSERT CMAKE_MODULE_PATH 0
    "${COMPILER_RT_SOURCE_DIR}/cmake"
    "${COMPILER_RT_SOURCE_DIR}/cmake/Modules"
    "${LLVM_COMMON_CMAKE_UTILS}"
    "${LLVM_COMMON_CMAKE_UTILS}/Modules"
  )

  include(base-config-ix)
  include(CompilerRTUtils)
  include(DetectWindowsItanium)

  if (NOT LLVM_RUNTIMES_BUILD)
    load_llvm_config()
  endif()
  construct_compiler_rt_default_triple()

  include(SetPlatformToolchainTools)
  include(AddCompilerRT)
endif()

if (NOT COMPILER_RT_STANDALONE_BUILD)
  include(AddCompilerRT)
endif()

if(NOT WIN32)
  message(STATUS "wincrt: Skipping (not Windows target)")
  return()
endif()

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(STATUS "wincrt: Skipping (requires Clang compiler)")
  return()
endif()

if(NOT TARGET wincrt)
  add_compiler_rt_component(wincrt)
endif()

#===------------------------------------------------------------------------===#
# Source files
#===------------------------------------------------------------------------===#

# Entry points are split into separate translation units so the linker only
# pulls in the entry point actually needed by the application/DLL.
set(WINCRT_ENTRY_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/entry_main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/entry_wmain.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/entry_winmain.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/entry_wwinmain.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/entry_dll.cpp
)

set(WINCRT_CORE_SOURCES
  ${WINCRT_ENTRY_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/init.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/security.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/cfguard.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/loadconfig.cpp
)

set(WINCRT_ITANIUM_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/cxa_atexit.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/cxa_thread_atexit.cpp
)

# Note: pseudo_reloc is in builtins (crt_pseudo_reloc_windows.cpp).

set(WINCRT_LINKTIME_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/purecall.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/delayload.cpp
)

set(WINCRT_COMPAT_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/atexit.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/rtc_stubs.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/msvc_eh.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/dtor_call.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/tls.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/compat/c11threads.cpp
)

set(WINCRT_SOURCES
  ${WINCRT_CORE_SOURCES}
  ${WINCRT_ITANIUM_SOURCES}
  ${WINCRT_LINKTIME_SOURCES}
  ${WINCRT_COMPAT_SOURCES}
)

# dtor_call.cpp uses SEH (__try/__except) instead of C++ exceptions to avoid
# circular dependencies with libunwind/libc++abi. No special flags needed.

#===------------------------------------------------------------------------===#
# Compiler flags
#===------------------------------------------------------------------------===#

# Minimal runtime flags - no C++ features, but needs Windows SDK.
# Cannot use -ffreestanding since Windows headers require libc declarations.
set(WINCRT_CFLAGS
  -fno-exceptions
  -fno-rtti
  -fms-extensions
  -O2
  -Wno-incompatible-library-redeclaration
)

# Require Windows 10 RS5 (build 17763) for _register_thread_local_exe_atexit_callback.
list(APPEND WINCRT_CFLAGS
  -D_WIN32_WINNT=0x0A00
  -DNTDDI_VERSION=0x0A000005
  -DWINVER=0x0A00
)

if(CMAKE_C_SIMULATE_ID STREQUAL "MSVC" OR MSVC)
  list(APPEND WINCRT_CFLAGS /Zl)
endif()

# Security hardening.
if(CMAKE_C_SIMULATE_ID STREQUAL "MSVC" OR MSVC)
  include(CheckCCompilerFlag)
  check_c_compiler_flag("/guard:cf" COMPILER_RT_HAS_GUARD_CF)
  if(COMPILER_RT_HAS_GUARD_CF)
    list(APPEND WINCRT_CFLAGS /guard:cf)
  endif()
  check_c_compiler_flag("/guard:ehcont" COMPILER_RT_HAS_GUARD_EHCONT)
  if(COMPILER_RT_HAS_GUARD_EHCONT)
    list(APPEND WINCRT_CFLAGS /guard:ehcont)
  endif()
endif()

#===------------------------------------------------------------------------===#
# Architecture detection
#===------------------------------------------------------------------------===#

if(DEFINED BUILTIN_SUPPORTED_ARCH)
  set(WINCRT_SUPPORTED_ARCH ${BUILTIN_SUPPORTED_ARCH})
else()
  set(WINCRT_SUPPORTED_ARCH)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|X64")
      list(APPEND WINCRT_SUPPORTED_ARCH x86_64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
      list(APPEND WINCRT_SUPPORTED_ARCH aarch64)
    endif()
  else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|i[3-6]86|X86")
      list(APPEND WINCRT_SUPPORTED_ARCH i386)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM")
      list(APPEND WINCRT_SUPPORTED_ARCH arm)
    endif()
  endif()
endif()

set(WINCRT_ARCH)
foreach(arch ${WINCRT_SUPPORTED_ARCH})
  if(arch MATCHES "x86_64|i386|i686|aarch64|arm")
    list(APPEND WINCRT_ARCH ${arch})
  endif()
endforeach()

if(NOT WINCRT_ARCH)
  message(STATUS "wincrt: No supported Windows architectures found")
  return()
endif()

# Register architectures using the standard compiler-rt pattern.
# add_default_target_arch sets CAN_TARGET_${arch} and COMPILER_RT_SUPPORTED_ARCH.
if(COMPILER_RT_WINCRT_STANDALONE_BUILD)
  foreach(arch ${WINCRT_ARCH})
    add_default_target_arch(${arch})
  endforeach()
endif()

#===------------------------------------------------------------------------===#
# Build the library
#===------------------------------------------------------------------------===#

add_compiler_rt_runtime(clang_rt.wincrt
  STATIC
  ARCHS ${WINCRT_ARCH}
  SOURCES ${WINCRT_SOURCES}
  CFLAGS ${WINCRT_CFLAGS}
  PARENT_TARGET wincrt)

# Only add compiler-rt dependency when not building standalone.
if(NOT COMPILER_RT_WINCRT_STANDALONE_BUILD AND TARGET compiler-rt)
  add_dependencies(compiler-rt wincrt)
endif()
