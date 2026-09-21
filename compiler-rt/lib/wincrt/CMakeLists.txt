# wincrt: CRT startup for Windows Itanium, replacing vcruntime on top of UCRT.
#
# Built before the C++ runtimes, which link it for their own DLL startup.
# cxa_atexit.cpp and cxa_thread_atexit.cpp are also compiled into the shared
# libc++ (see libcxx.cmake), which then owns the process-wide registries.

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
else()
  include(AddCompilerRT)
endif()

if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
  message(STATUS "wincrt: Skipping (requires Clang targeting Windows)")
  return()
endif()

if(NOT TARGET wincrt)
  add_compiler_rt_component(wincrt)
endif()

set(WINCRT_SOURCES
  # Entry points are separate translation units so the linker extracts only
  # the one named by /entry.
  ${CMAKE_CURRENT_SOURCE_DIR}/entry_main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/entry_wmain.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/entry_winmain.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/entry_wwinmain.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/entry_dll.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/init.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/atexit.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/cxa_atexit.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/cxa_thread_atexit.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tls.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/security.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/gshandler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/cfguard.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/cfguard_dispatch.S
  ${CMAKE_CURRENT_SOURCE_DIR}/loadconfig.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/purecall.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/delayload.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/weak_interposition.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ucrt_stdio_options.c
  ${CMAKE_CURRENT_SOURCE_DIR}/ucrt_stdio_marker.c
)

# SEH, __declspec(allocate) and linker directives need -fms-extensions. The
# library uses no C++ exceptions or RTTI itself.
set(WINCRT_CFLAGS
  -fno-exceptions
  -fno-rtti
  -fms-extensions
  -O2
  -D_WIN32_WINNT=0x0A00
  -DWINVER=0x0A00
  -DNTDDI_VERSION=0x0A000005
)

# Runtime callbacks are indirect calls; instrument them like application code.
include(CheckCXXCompilerFlag)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set(WINCRT_GUARD_CF_FLAG /guard:cf)
else()
  set(WINCRT_GUARD_CF_FLAG -mguard=cf)
endif()
check_cxx_compiler_flag("${WINCRT_GUARD_CF_FLAG}" COMPILER_RT_WINCRT_HAS_GUARD_CF)
if(COMPILER_RT_WINCRT_HAS_GUARD_CF)
  list(APPEND WINCRT_CFLAGS ${WINCRT_GUARD_CF_FLAG})
endif()

if(DEFINED BUILTIN_SUPPORTED_ARCH)
  set(WINCRT_CANDIDATE_ARCH ${BUILTIN_SUPPORTED_ARCH})
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|X64")
    set(WINCRT_CANDIDATE_ARCH x86_64)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
    set(WINCRT_CANDIDATE_ARCH aarch64)
  endif()
endif()

set(WINCRT_ARCH)
foreach(arch ${WINCRT_CANDIDATE_ARCH})
  if(arch MATCHES "^(x86_64|aarch64)$")
    list(APPEND WINCRT_ARCH ${arch})
  endif()
endforeach()
if(NOT WINCRT_ARCH)
  message(STATUS "wincrt: No supported architecture (x86_64, aarch64)")
  return()
endif()

if(COMPILER_RT_WINCRT_STANDALONE_BUILD)
  foreach(arch ${WINCRT_ARCH})
    add_default_target_arch(${arch})
  endforeach()
endif()

add_compiler_rt_runtime(clang_rt.wincrt
  STATIC
  ARCHS ${WINCRT_ARCH}
  SOURCES ${WINCRT_SOURCES}
  CFLAGS ${WINCRT_CFLAGS}
  PARENT_TARGET wincrt)

if(NOT COMPILER_RT_WINCRT_STANDALONE_BUILD AND TARGET compiler-rt)
  add_dependencies(compiler-rt wincrt)
endif()

# ucrt_memory.def lists functions that ucrtbase.dll exports but ucrt.lib does
# not (Microsoft provides them through vcruntime.lib). The generated import
# library is installed beside clang_rt.wincrt.lib; the driver adds it to every
# compiler-rt link, so user programs never need this build's CMake.
find_program(WINCRT_LIB_TOOL NAMES llvm-lib
  HINTS ${LLVM_TOOLS_BINARY_DIR} ${LLVM_RUNTIME_OUTPUT_INTDIR})
if(NOT WINCRT_LIB_TOOL)
  message(FATAL_ERROR "wincrt: llvm-lib is required to generate clang_rt.ucrt_memory.lib")
endif()

set(WINCRT_UCRT_MEMORY_DEF "${CMAKE_CURRENT_SOURCE_DIR}/ucrt_memory.def")
foreach(arch ${WINCRT_ARCH})
  if(arch STREQUAL "x86_64")
    set(machine X64)
  else()
    set(machine ARM64)
  endif()

  get_compiler_rt_output_dir(${arch} output_dir)
  get_compiler_rt_install_dir(${arch} install_dir)
  set(import_lib "${output_dir}/clang_rt.ucrt_memory.lib")

  add_custom_command(OUTPUT "${import_lib}"
    COMMAND ${CMAKE_COMMAND} -E make_directory ${output_dir}
    COMMAND ${WINCRT_LIB_TOOL} /nologo /def:${WINCRT_UCRT_MEMORY_DEF}
            /out:${import_lib} /machine:${machine}
    DEPENDS "${WINCRT_UCRT_MEMORY_DEF}"
    COMMENT "Generating clang_rt.ucrt_memory.lib (${arch})"
    VERBATIM)
  add_custom_target(clang_rt.ucrt_memory-${arch} ALL DEPENDS "${import_lib}")
  add_dependencies(wincrt clang_rt.ucrt_memory-${arch})
  install(FILES "${import_lib}" DESTINATION "${install_dir}" COMPONENT wincrt)
endforeach()
