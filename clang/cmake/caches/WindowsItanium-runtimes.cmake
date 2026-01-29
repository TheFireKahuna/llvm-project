# WindowsItanium-runtimes.cmake - Build Clang and Windows Itanium runtimes
#
# Stage 1 of 2: Builds Clang/LLD/LLDB plus runtimes (libunwind, libc++abi,
# libc++) targeting Windows Itanium. The compiler produced uses the host ABI
# (MSVC) but can target Windows Itanium. Bootstraps to stage 2 for a
# self-hosted native build.
#
# See: https://llvm.org/docs/HowToBuildWindowsItaniumPrograms.html
#
# Prerequisites:
#   - C++ compiler: MSVC (cl.exe) or Clang (clang-cl.exe)
#   - Visual Studio with Windows SDK (for headers/libs)
#   - CMake 3.20+, Ninja, Python 3, Git
#
# Setup (run once per shell session):
#   # PowerShell - find and load VS dev environment:
#   $vsPath = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
#             -latest -property installationPath
#   & "$vsPath\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -SkipAutomaticLocation
#
# Build (produces native Windows Itanium toolchain):
#   # Using MSVC:
#   cmake -G Ninja -B build -C <path>/clang/cmake/caches/WindowsItanium-runtimes.cmake \
#         -DCMAKE_INSTALL_PREFIX=<install-path> <path>/llvm
#
#   # Or using Clang:
#   cmake -G Ninja -B build -C <path>/clang/cmake/caches/WindowsItanium-runtimes.cmake \
#         -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl \
#         -DCMAKE_INSTALL_PREFIX=<install-path> <path>/llvm
#
#   ninja -C build stage2-distribution
#   ninja -C build stage2-install-distribution
#
# The final native Windows Itanium toolchain will be in <install-path>.

cmake_minimum_required(VERSION 3.20)

set(PACKAGE_VENDOR "Windows-Itanium" CACHE STRING "")

#===------------------------------------------------------------------------===#
# Compiler Selection - Use clang-cl or MSVC (both produce MSVC ABI binaries)
#===------------------------------------------------------------------------===#
if(NOT DEFINED CMAKE_C_COMPILER)
  # Prefer clang-cl if available, fall back to MSVC cl.exe
  find_program(_WI_HOST_CC NAMES clang-cl cl)
  if(_WI_HOST_CC)
    set(CMAKE_C_COMPILER "${_WI_HOST_CC}" CACHE FILEPATH "")
    set(CMAKE_CXX_COMPILER "${_WI_HOST_CC}" CACHE FILEPATH "")
    message(STATUS "Using host compiler: ${_WI_HOST_CC}")
  else()
    message(FATAL_ERROR "No suitable C++ compiler found. Install MSVC or Clang.")
  endif()
endif()

#===------------------------------------------------------------------------===#
# Build Dependencies with MSVC ABI (for this phase's toolchain)
#===------------------------------------------------------------------------===#
# Build dependencies with MSVC ABI using the host compiler (clang-cl or cl.exe).
# Stage 2 will build its own Windows Itanium ABI versions using the WI driver.

include(${CMAKE_CURRENT_LIST_DIR}/WindowsItanium-toolchain.cmake)

wi_build_all_dependencies(
  COMPILER "${CMAKE_C_COMPILER}"
  CXX_COMPILER "${CMAKE_CXX_COMPILER}"
  BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}"
  ABI_SUFFIX "-msvc"
  TARGET ""  # No target = native MSVC ABI
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
# Stage 1: Clang + Runtimes (host-ABI compiler targeting WI)
#===------------------------------------------------------------------------===#

set(LLVM_TARGETS_TO_BUILD "X86" CACHE STRING "")

# Note: LLVM_ENABLE_PROJECTS and LLVM_ENABLE_RUNTIMES are auto-passed to stage2
# via _BOOTSTRAP_DEFAULT_PASSTHROUGH in clang/CMakeLists.txt.
set(LLVM_ENABLE_PROJECTS "clang;clang-tools-extra;lld;lldb" CACHE STRING "")
set(LLVM_ENABLE_RUNTIMES "libunwind;libcxxabi;libcxx" CACHE STRING "")

# Build runtimes for Windows Itanium target.
set(LLVM_RUNTIME_TARGETS "x86_64-unknown-windows-itanium" CACHE STRING "")

set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(LLVM_ENABLE_LLD ON CACHE BOOL "")

# Enable features needed for full toolchain.
set(CLANG_ENABLE_STATIC_ANALYZER ON CACHE BOOL "")
set(LLDB_ENABLE_CURSES OFF CACHE BOOL "")
set(LLDB_ENABLE_LIBEDIT OFF CACHE BOOL "")
set(LLVM_ENABLE_BACKTRACES OFF CACHE BOOL "")
set(LLVM_ENABLE_LIBEDIT OFF CACHE BOOL "")
# Prefer CMake config-file packages over Find modules so our built dependencies
# are used instead of system-installed ones. The libxml2 config file correctly
# sets LIBXML_STATIC for static builds.
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON CACHE BOOL "")
set(LLVM_ENABLE_LIBXML2 ON CACHE BOOL "")
set(LLVM_ENABLE_Z3_SOLVER OFF CACHE BOOL "")
set(LLVM_ENABLE_ZLIB ON CACHE BOOL "")
set(LLVM_ENABLE_ZSTD ON CACHE BOOL "")
set(LLVM_INCLUDE_DOCS OFF CACHE BOOL "")
set(LLVM_INCLUDE_EXAMPLES OFF CACHE BOOL "")

# Runtime configuration is handled by HandleWindowsItaniumDefaults.cmake,
# which is automatically included for the windows-itanium triple.
# Use RelWithDebInfo for runtimes to enable debugging into libc++ if needed.
set(RUNTIMES_x86_64-unknown-windows-itanium_CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "")

#===------------------------------------------------------------------------===#
# Variables to pass through to Stage 2 (native build)
#===------------------------------------------------------------------------===#
# These variables are set once here and automatically passed to stage2.
# See clang/CMakeLists.txt for _BOOTSTRAP_DEFAULT_PASSTHROUGH (auto-passed:
# PACKAGE_VENDOR, CMAKE_BUILD_TYPE, LLVM_ENABLE_PROJECTS, LLVM_ENABLE_RUNTIMES).

set(CLANG_BOOTSTRAP_PASSTHROUGH
  LLVM_TARGETS_TO_BUILD
  LLVM_RUNTIME_TARGETS
  LLVM_ENABLE_LLD
  LLVM_ENABLE_ZLIB
  LLVM_ENABLE_ZSTD
  LLVM_ENABLE_LIBXML2
  LLVM_ENABLE_Z3_SOLVER
  LLVM_ENABLE_BACKTRACES
  LLVM_ENABLE_LIBEDIT
  LLVM_INCLUDE_DOCS
  LLVM_INCLUDE_EXAMPLES
  CLANG_ENABLE_STATIC_ANALYZER
  LLDB_ENABLE_CURSES
  LLDB_ENABLE_LIBEDIT
  CMAKE_FIND_PACKAGE_PREFER_CONFIG
  RUNTIMES_x86_64-unknown-windows-itanium_CMAKE_BUILD_TYPE
  CACHE STRING "")

#===------------------------------------------------------------------------===#
# Stage 2-only settings (BOOTSTRAP_ prefix)
#===------------------------------------------------------------------------===#
# These settings apply only to the native Windows Itanium build.

# Build LLVM/Clang using libc++. The resulting binaries depend on c++.dll
# and unwind.dll at runtime (distributed with the toolchain).
set(BOOTSTRAP_LLVM_ENABLE_LIBCXX ON CACHE BOOL "")

# Optimizations for stage2.
set(BOOTSTRAP_LLVM_ENABLE_LTO Thin CACHE STRING "")
set(BOOTSTRAP_CLANG_PLUGIN_SUPPORT OFF CACHE BOOL "")
set(BOOTSTRAP_ENABLE_LINKER_BUILD_ID ON CACHE BOOL "")
set(BOOTSTRAP_ENABLE_X86_RELAX_RELOCATIONS ON CACHE BOOL "")
set(BOOTSTRAP_LLVM_ENABLE_PLUGINS OFF CACHE BOOL "")
set(BOOTSTRAP_LLVM_ENABLE_UNWIND_TABLES OFF CACHE BOOL "")
set(BOOTSTRAP_LLVM_USE_RELATIVE_PATHS_IN_FILES ON CACHE BOOL "")

# Force HAVE_LIBXML2=TRUE to skip check_symbol_exists in stage2. We build
# libxml2 ourselves, so we know it's valid.
set(BOOTSTRAP_HAVE_LIBXML2 TRUE CACHE BOOL "")

# Distribution components for stage2.
set(BOOTSTRAP_LLVM_INSTALL_TOOLCHAIN_ONLY OFF CACHE BOOL "")

set(BOOTSTRAP_LLVM_TOOLCHAIN_TOOLS
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

set(BOOTSTRAP_LLVM_DISTRIBUTION_COMPONENTS
  clang
  clang-format
  clang-resource-headers
  clang-tidy
  clangd
  lld
  lldb
  LTO
  runtimes
  ${BOOTSTRAP_LLVM_TOOLCHAIN_TOOLS}
  CACHE STRING "")

#===------------------------------------------------------------------------===#
# Bootstrap to Stage 2
#===------------------------------------------------------------------------===#

set(CLANG_ENABLE_BOOTSTRAP ON CACHE BOOL "")

# Build bootstrap cmake args for stage 2 (native Windows Itanium build).
# Stage 2 will build its own Windows Itanium ABI dependencies.
# Pass the stage 1 build directory so stage 2 can find runtimes and tools.
# Note: -C cache files run BEFORE -D options, so we must pass this via -D.
set(CLANG_BOOTSTRAP_CMAKE_ARGS
  -D_WI_PHASE1_BUILD_DIR=${CMAKE_BINARY_DIR}
  -C ${CMAKE_CURRENT_LIST_DIR}/WindowsItanium-native.cmake
  CACHE STRING "")

# Note: LLVM_ENABLE_PROJECTS and LLVM_ENABLE_RUNTIMES are auto-passed via
# _BOOTSTRAP_DEFAULT_PASSTHROUGH, no need to set BOOTSTRAP_ versions.

# Tell bootstrap to use GNU-style driver (clang/clang++) instead of clang-cl.
set(BOOTSTRAP_LLVM_HOST_TRIPLE "x86_64-unknown-windows-itanium" CACHE STRING "")

# Targets to expose through the stage 1 build.
set(CLANG_BOOTSTRAP_TARGETS
  check-all
  check-clang
  check-lld
  check-llvm
  distribution
  install-distribution
  install-distribution-stripped
  CACHE STRING "")
