# WindowsItanium-native.cmake - Build native Windows Itanium LLVM toolchain
#
# Stage 2 of 2: Builds Clang/LLD/LLDB as native Windows Itanium binaries,
# linked against libc++. The resulting clang.exe itself uses the Itanium ABI.
#
# This phase is typically invoked automatically via bootstrap from
# WindowsItanium-runtimes.cmake. It can also be used standalone if you have:
#   1. A Clang that can target Windows Itanium
#   2. Windows Itanium runtimes (libunwind, libc++abi, libc++) installed
#
# See: https://llvm.org/docs/HowToBuildWindowsItaniumPrograms.html
#
# Prerequisites (for standalone use):
#   - Clang compiler with Windows Itanium support
#   - Windows Itanium runtimes installed (in compiler search paths or via
#     CMAKE_PREFIX_PATH pointing to install location)
#   - Visual Studio with Windows SDK (for headers/libs)
#   - CMake 3.20+, Ninja, Python 3, Git
#
# Standalone build:
#   cmake -G Ninja -B build -C <path>/clang/cmake/caches/WindowsItanium-native.cmake \
#         -DCMAKE_C_COMPILER=<clang> -DCMAKE_CXX_COMPILER=<clang++> \
#         -DCMAKE_PREFIX_PATH=<runtimes-install-path> \
#         -DCMAKE_INSTALL_PREFIX=<install-path> <path>/llvm
#   ninja -C build distribution
#   ninja -C build install-distribution
#
# The native Windows Itanium toolchain will be in <install-path>.
#
# Note: Most settings are inherited from WindowsItanium-runtimes.cmake via
# CLANG_BOOTSTRAP_PASSTHROUGH and BOOTSTRAP_ prefixed variables. This file
# contains only stage2-specific configuration that cannot be passed through.

cmake_minimum_required(VERSION 3.20)

#===------------------------------------------------------------------------===#
# Compiler Selection
#===------------------------------------------------------------------------===#
# Windows Itanium uses GNU-style driver (clang/clang++), not clang-cl.
# The WindowsItaniumToolChain expects GNU-style flags.

if(NOT DEFINED CMAKE_C_COMPILER)
  find_program(_WI_CLANG NAMES clang REQUIRED)
  set(CMAKE_C_COMPILER "${_WI_CLANG}" CACHE FILEPATH "")
endif()

if(NOT DEFINED CMAKE_CXX_COMPILER)
  find_program(_WI_CLANGXX NAMES clang++ REQUIRED)
  set(CMAKE_CXX_COMPILER "${_WI_CLANGXX}" CACHE FILEPATH "")
endif()

#===------------------------------------------------------------------------===#
# Build Dependencies with Windows Itanium ABI
#===------------------------------------------------------------------------===#
# Build dependencies with Windows Itanium ABI using clang/clang++.
# These are used to build the native WI toolchain itself.
#
# Note: Do NOT use vcpkg here - vcpkg packages are MSVC ABI, which is
# incompatible with Windows Itanium.

include(${CMAKE_CURRENT_LIST_DIR}/WindowsItanium-toolchain.cmake)

# Find compilers for dependency builds - use clang from stage 1 build.
# The stage 1 clang has the WindowsItaniumToolChain which automatically uses LLD.
#
# When bootstrapping from stage 1, _WI_PHASE1_BUILD_DIR is passed via -D option.
# For standalone use (not bootstrapping), fall back to CMAKE_C_COMPILER directory.
if(DEFINED _WI_PHASE1_BUILD_DIR)
  set(_WI_COMPILER_DIR "${_WI_PHASE1_BUILD_DIR}/bin")
  set(_WI_PHASE1_LIB "${_WI_PHASE1_BUILD_DIR}/lib")
  message(STATUS "Using stage 1 build directory: ${_WI_PHASE1_BUILD_DIR}")
else()
  get_filename_component(_WI_COMPILER_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
  get_filename_component(_WI_COMPILER_DIR "${_WI_COMPILER_DIR}" ABSOLUTE)
  get_filename_component(_WI_PHASE1_ROOT "${_WI_COMPILER_DIR}" DIRECTORY)
  set(_WI_PHASE1_LIB "${_WI_PHASE1_ROOT}/lib")
  message(STATUS "Standalone mode - using compiler directory: ${_WI_COMPILER_DIR}")
endif()
message(STATUS "Looking for clang/clang++ in: ${_WI_COMPILER_DIR}")
unset(_WI_DEP_CC CACHE)
unset(_WI_DEP_CXX CACHE)
find_program(_WI_DEP_CC NAMES clang HINTS "${_WI_COMPILER_DIR}" NO_DEFAULT_PATH REQUIRED)
find_program(_WI_DEP_CXX NAMES clang++ HINTS "${_WI_COMPILER_DIR}" NO_DEFAULT_PATH REQUIRED)
message(STATUS "Found clang: ${_WI_DEP_CC}")
message(STATUS "Found clang++: ${_WI_DEP_CXX}")

# Use llvm-ar/llvm-ranlib from the stage 1 compiler's bin directory instead of
# system-installed llvm-lib/lib.exe. This avoids version mismatches.
unset(_WI_AR CACHE)
unset(_WI_RANLIB CACHE)
find_program(_WI_AR NAMES llvm-ar HINTS "${_WI_COMPILER_DIR}" NO_DEFAULT_PATH REQUIRED)
find_program(_WI_RANLIB NAMES llvm-ranlib HINTS "${_WI_COMPILER_DIR}" NO_DEFAULT_PATH REQUIRED)
set(CMAKE_AR "${_WI_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_WI_RANLIB}" CACHE FILEPATH "" FORCE)

wi_build_all_dependencies(
  COMPILER "${_WI_DEP_CC}"
  CXX_COMPILER "${_WI_DEP_CXX}"
  BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}"
  ABI_SUFFIX "-wi"
  TARGET "x86_64-unknown-windows-itanium"
)

# Add dependency install directories to CMAKE_PREFIX_PATH so find_package can
# locate them in sub-builds (runtimes external project).
set(CMAKE_PREFIX_PATH "${ZLIB_ROOT};${zstd_ROOT};${LibXml2_ROOT}" CACHE PATH "")

# Pass CMAKE_PREFIX_PATH and *_ROOT variables to runtimes external project.
set(LLVM_EXTERNAL_PROJECT_PASSTHROUGH
  CMAKE_PREFIX_PATH
  ZLIB_ROOT
  zstd_ROOT
  LibXml2_ROOT
  CACHE STRING "")

# Set explicit paths for dependencies that get passed through to runtimes via
# LLVMExternalProjectUtils DEFAULT_PASSTHROUGH_VARIABLES.
set(ZLIB_LIBRARY "${ZLIB_ROOT}/lib/zlibstatic.lib" CACHE FILEPATH "")
set(ZLIB_INCLUDE_DIR "${ZLIB_ROOT}/include" CACHE PATH "")
set(zstd_LIBRARY "${zstd_ROOT}/lib/zstd_static.lib" CACHE FILEPATH "")
set(zstd_INCLUDE_DIR "${zstd_ROOT}/include" CACHE PATH "")

#===------------------------------------------------------------------------===#
# Native Windows Itanium Build Configuration
#===------------------------------------------------------------------------===#
# These settings are specific to the native build and cannot be passed through.

# Target configuration - this clang.exe will BE a Windows Itanium binary.
set(LLVM_DEFAULT_TARGET_TRIPLE "x86_64-unknown-windows-itanium" CACHE STRING "")
set(LLVM_HOST_TRIPLE "x86_64-unknown-windows-itanium" CACHE STRING "")

# Tell CMake to compile for Windows Itanium target.
set(CMAKE_C_COMPILER_TARGET "x86_64-unknown-windows-itanium" CACHE STRING "")
set(CMAKE_CXX_COMPILER_TARGET "x86_64-unknown-windows-itanium" CACHE STRING "")

#===------------------------------------------------------------------------===#
# Compiler Flags
#===------------------------------------------------------------------------===#

# Suppress warnings for Microsoft extensions in LLVM headers.
set(CMAKE_C_FLAGS "-Wno-language-extension-token -Wno-microsoft-enum-value" CACHE STRING "")
set(CMAKE_CXX_FLAGS "-Wno-language-extension-token -Wno-microsoft-enum-value" CACHE STRING "")

# Add library path and libraries for stage 1 runtimes.
# CMake's try_compile tests need to find and link c++.lib and unwind.lib.
# Use -L (GNU-style) which the WindowsItanium toolchain converts to -libpath:.
# Also explicitly add libraries since CMake may pass -nostdlib which suppresses
# the toolchain's automatic -defaultlib: additions.
message(STATUS "Looking for stage 1 runtimes in: ${_WI_PHASE1_LIB}")
if(EXISTS "${_WI_PHASE1_LIB}/c++.lib")
  message(STATUS "Found stage 1 runtimes: ${_WI_PHASE1_LIB}/c++.lib")
  set(CMAKE_EXE_LINKER_FLAGS "-L\"${_WI_PHASE1_LIB}\" -lc++ -lunwind" CACHE STRING "" FORCE)
  set(CMAKE_SHARED_LINKER_FLAGS "-L\"${_WI_PHASE1_LIB}\" -lc++ -lunwind" CACHE STRING "" FORCE)
else()
  message(STATUS "Stage 1 runtimes NOT found at: ${_WI_PHASE1_LIB}/c++.lib")
  message(STATUS "CMake try_compile may fail - set CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY as workaround")
endif()

#===------------------------------------------------------------------------===#
# Bootstrap Termination
#===------------------------------------------------------------------------===#

# This is the final stage - no further bootstrap.
set(CLANG_ENABLE_BOOTSTRAP OFF CACHE BOOL "")

#===------------------------------------------------------------------------===#
# Standalone Mode Defaults
#===------------------------------------------------------------------------===#
# These are only used when running standalone (not via bootstrap).
# When bootstrapping, these are overridden by values passed from stage 1.

if(NOT DEFINED PACKAGE_VENDOR)
  set(PACKAGE_VENDOR "Windows-Itanium" CACHE STRING "")
endif()

if(NOT DEFINED LLVM_TARGETS_TO_BUILD)
  set(LLVM_TARGETS_TO_BUILD "X86" CACHE STRING "")
endif()

if(NOT DEFINED LLVM_ENABLE_PROJECTS)
  set(LLVM_ENABLE_PROJECTS "clang;clang-tools-extra;lld;lldb" CACHE STRING "")
endif()

if(NOT DEFINED LLVM_ENABLE_RUNTIMES)
  set(LLVM_ENABLE_RUNTIMES "libunwind;libcxxabi;libcxx" CACHE STRING "")
endif()

if(NOT DEFINED LLVM_RUNTIME_TARGETS)
  set(LLVM_RUNTIME_TARGETS "x86_64-unknown-windows-itanium" CACHE STRING "")
endif()

if(NOT DEFINED CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "")
endif()

if(NOT DEFINED LLVM_ENABLE_LLD)
  set(LLVM_ENABLE_LLD ON CACHE BOOL "")
endif()

if(NOT DEFINED LLVM_ENABLE_LIBCXX)
  set(LLVM_ENABLE_LIBCXX ON CACHE BOOL "")
endif()

if(NOT DEFINED LLVM_ENABLE_LTO)
  set(LLVM_ENABLE_LTO Thin CACHE STRING "")
endif()

if(NOT DEFINED RUNTIMES_x86_64-unknown-windows-itanium_CMAKE_BUILD_TYPE)
  set(RUNTIMES_x86_64-unknown-windows-itanium_CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "")
endif()

# Standalone distribution components (used when not bootstrapping).
if(NOT DEFINED LLVM_DISTRIBUTION_COMPONENTS)
  set(LLVM_TOOLCHAIN_TOOLS
    llvm-ar
    llvm-cov
    llvm-cxxfilt
    llvm-dlltool
    llvm-dwarfdump
    llvm-dwp
    llvm-gsymutil
    llvm-ifs
    llvm-lib
    llvm-ml
    llvm-mt
    llvm-nm
    llvm-objcopy
    llvm-objdump
    llvm-pdbutil
    llvm-profdata
    llvm-ranlib
    llvm-rc
    llvm-readelf
    llvm-readobj
    llvm-size
    llvm-strings
    llvm-strip
    llvm-symbolizer
    llvm-undname
    llvm-xray
    CACHE STRING "")

  set(LLVM_DISTRIBUTION_COMPONENTS
    clang
    clang-format
    clang-resource-headers
    clang-tidy
    clangd
    lld
    lldb
    LTO
    runtimes
    ${LLVM_TOOLCHAIN_TOOLS}
    CACHE STRING "")
endif()
