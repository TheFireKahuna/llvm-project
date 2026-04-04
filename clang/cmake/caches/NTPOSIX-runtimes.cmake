# NTPOSIX-runtimes.cmake - Build Clang and NTPOSIX runtimes
#
# Stage 1 of 2: Builds Clang/LLD plus runtimes (compiler-rt, libunwind,
# libc++abi, libc++, llvm-libc) targeting NTPOSIX (POSIX-on-NT). The compiler
# produced uses the host ABI (MSVC) but can target NTPOSIX. Bootstraps to
# stage 2 for a self-hosted native build.
#
# NTPOSIX is POSIX-on-NT: Itanium ABI, compiler-rt, libunwind, libc++,
# llvm-libc as the sole C runtime, lld-link, PE/COFF. No UCRT dependency.
#
# Prerequisites:
#   - LLVM Clang (clang-cl.exe) on PATH or passed via -DCMAKE_C_COMPILER
#   - CMake 3.20+, Ninja, Python 3, Git
#
# Build (produces native NTPOSIX toolchain):
#   cmake -G Ninja -B build -C <path>/clang/cmake/caches/NTPOSIX-runtimes.cmake \
#         -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl \
#         -DCMAKE_INSTALL_PREFIX=<install-path> <path>/llvm
#
#   ninja -C build stage2-distribution
#   ninja -C build stage2-install-distribution
#
# The final native NTPOSIX toolchain will be in <install-path>.

set(PACKAGE_VENDOR "NTPOSIX" CACHE STRING "")
# Windows 10/11 with long paths enabled can tolerate much deeper object paths
# than CMake's conservative default.
set(CMAKE_OBJECT_PATH_MAX 32768 CACHE STRING "")

# ---
# Compiler Selection - Use clang-cl for the host ABI build
# ---
if(NOT DEFINED CMAKE_C_COMPILER)
  find_program(_WI_HOST_CC NAMES clang-cl)
  if(_WI_HOST_CC)
    set(CMAKE_C_COMPILER "${_WI_HOST_CC}" CACHE FILEPATH "")
    set(CMAKE_CXX_COMPILER "${_WI_HOST_CC}" CACHE FILEPATH "")
  else()
    message(FATAL_ERROR "clang-cl.exe not found. Install LLVM Clang and/or pass -DCMAKE_C_COMPILER/-DCMAKE_CXX_COMPILER explicitly.")
  endif()
endif()

# ---
# Build Dependencies with the host ABI (for this phase's toolchain)
# ---
# Build dependencies with the host ABI using clang-cl.
# Stage 2 will build its own NTPOSIX ABI versions using the NTPOSIX driver.

include(${CMAKE_CURRENT_LIST_DIR}/WindowsItanium-toolchain.cmake)
wi_configure_host_masm("${CMAKE_C_COMPILER}")

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
set(LIBXML2_LIBRARY "${LibXml2_ROOT}/lib/libxml2.lib" CACHE FILEPATH "")
set(LIBXML2_INCLUDE_DIR "${LibXml2_ROOT}/include/libxml2" CACHE PATH "")
set(LibXml2_DIR "${LibXml2_ROOT}/lib/cmake/libxml2-${_WI_LIBXML2_VERSION}" CACHE PATH "")

# ---
# Stage 1: Clang + Runtimes (host-ABI compiler targeting NTPOSIX)
# ---

set(LLVM_TARGETS_TO_BUILD "X86" CACHE STRING "")

# Note: LLVM_ENABLE_PROJECTS and LLVM_ENABLE_RUNTIMES are auto-passed to stage2
# via _BOOTSTRAP_DEFAULT_PASSTHROUGH in clang/CMakeLists.txt.
set(LLVM_ENABLE_PROJECTS "clang;clang-tools-extra;lld" CACHE STRING "")
set(LLVM_ENABLE_RUNTIMES "compiler-rt;libunwind;libcxxabi;libcxx;libc" CACHE STRING "")

# Build runtimes for NTPOSIX target.
set(LLVM_RUNTIME_TARGETS "x86_64-pc-windows-ntposix" CACHE STRING "")
# Build target-specific builtins to get BUILTINS_<target>_ variable unpacking.
set(LLVM_BUILTIN_TARGETS "x86_64-pc-windows-ntposix" CACHE STRING "")

# Toolchain file for stage 1 runtimes/builtins: overrides Windows-GNU.cmake
# platform rules with MSVC-style linking (lld-link) and sets RC compiler to
# llvm-rc. Without this, CMake detects the GNU-style driver as MinGW.
get_filename_component(_WI_TOOLCHAIN
  "${CMAKE_CURRENT_LIST_DIR}/../../../llvm/cmake/modules/WindowsItaniumToolchain.cmake"
  ABSOLUTE)
set(RUNTIMES_x86_64-pc-windows-ntposix_CMAKE_TOOLCHAIN_FILE
  "${_WI_TOOLCHAIN}" CACHE FILEPATH "")
set(BUILTINS_x86_64-pc-windows-ntposix_CMAKE_TOOLCHAIN_FILE
  "${_WI_TOOLCHAIN}" CACHE FILEPATH "")

# Also set CMAKE_USER_MAKE_RULES_OVERRIDE directly so it's available in the
# top-level configure (the toolchain file sets this too, but belt-and-suspenders).
get_filename_component(_WI_PLATFORM_MODULE
  "${CMAKE_CURRENT_LIST_DIR}/../../../llvm/cmake/modules/WindowsItaniumRules.cmake"
  ABSOLUTE)
if(EXISTS "${_WI_PLATFORM_MODULE}")
  set(CMAKE_USER_MAKE_RULES_OVERRIDE "${_WI_PLATFORM_MODULE}" CACHE FILEPATH "")
endif()
unset(_WI_PLATFORM_MODULE)

set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(LLVM_ENABLE_LLD ON CACHE BOOL "")

# Enable features needed for full toolchain.
set(CLANG_ENABLE_STATIC_ANALYZER ON CACHE BOOL "")
set(LLDB_ENABLE_CURSES OFF CACHE BOOL "")
set(LLDB_ENABLE_LIBEDIT OFF CACHE BOOL "")
set(LLVM_ENABLE_BACKTRACES OFF CACHE BOOL "")
set(LLVM_ENABLE_DIA_SDK OFF CACHE BOOL "")
set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "")
set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "")
# Prefer CMake config-file packages over Find modules so our built dependencies
# are used instead of system-installed ones (e.g. libxml2 LIBXML_STATIC).
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON CACHE BOOL "")
set(LLVM_ENABLE_LIBXML2 ON CACHE BOOL "")
set(LLVM_ENABLE_Z3_SOLVER OFF CACHE BOOL "")
set(LLVM_ENABLE_ZLIB ON CACHE BOOL "")
set(LLVM_ENABLE_ZSTD ON CACHE BOOL "")
set(LLVM_INCLUDE_DOCS OFF CACHE BOOL "")
set(LLVM_INCLUDE_EXAMPLES OFF CACHE BOOL "")

# Runtime configuration for NTPOSIX target.
# Use RelWithDebInfo for runtimes to enable debugging into libc++ if needed.
set(RUNTIMES_x86_64-pc-windows-ntposix_CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "")

# NTPOSIX: llvm-libc is the sole C runtime - enable full build.
set(RUNTIMES_x86_64-pc-windows-ntposix_LLVM_LIBC_FULL_BUILD ON CACHE BOOL "")

# NTPOSIX: the compiler driver injects our c.lib as the default libc, so the
# test link line never sees a foreign libc. Link unit tests as freestanding
# (same recipe as hermetic tests) so __llvm_libc_* public symbols resolve
# uniquely from the libc __internal__ objects instead of colliding with
# c.dll's exports.
set(RUNTIMES_x86_64-pc-windows-ntposix_LIBC_UNIT_TEST_LINK_FREESTANDING
    ON CACHE BOOL "")

# NTPOSIX: Always use compiler-rt instead of libgcc.
set(RUNTIMES_x86_64-pc-windows-ntposix_LIBUNWIND_USE_COMPILER_RT ON CACHE BOOL "")
set(RUNTIMES_x86_64-pc-windows-ntposix_LIBCXXABI_USE_COMPILER_RT ON CACHE BOOL "")
set(RUNTIMES_x86_64-pc-windows-ntposix_LIBCXX_USE_COMPILER_RT ON CACHE BOOL "")

# NTPOSIX: Use llvm-libc for runtime builds.
set(RUNTIMES_x86_64-pc-windows-ntposix_RUNTIMES_USE_LIBC "llvm-libc" CACHE STRING "")

# ---
# Variables to pass through to Stage 2 (native build)
# ---
# These variables are set once here and automatically passed to stage2.
# See clang/CMakeLists.txt for _BOOTSTRAP_DEFAULT_PASSTHROUGH (auto-passed:
# PACKAGE_VENDOR, CMAKE_BUILD_TYPE, LLVM_ENABLE_PROJECTS, LLVM_ENABLE_RUNTIMES).

set(CLANG_BOOTSTRAP_PASSTHROUGH
  LLVM_TARGETS_TO_BUILD
  LLVM_RUNTIME_TARGETS
  # Keep stage2 on the explicit NTPOSIX builtins layout; otherwise bootstrap
  # falls back to compiler-rt's generic default target and emits MinGW-shaped
  # builtins under lib/clang/<ver>/lib/windows/.
  LLVM_BUILTIN_TARGETS
  LLVM_ENABLE_LLD
  LLVM_ENABLE_ZLIB
  LLVM_ENABLE_ZSTD
  LLVM_ENABLE_LIBXML2
  LLVM_ENABLE_Z3_SOLVER
  LLVM_ENABLE_BACKTRACES
  LLVM_ENABLE_LIBEDIT
  LLVM_INCLUDE_BENCHMARKS
  LLVM_INCLUDE_TESTS
  LLVM_INCLUDE_DOCS
  LLVM_INCLUDE_EXAMPLES
  CLANG_ENABLE_STATIC_ANALYZER
  LLDB_ENABLE_CURSES
  LLDB_ENABLE_LIBEDIT
  CMAKE_FIND_PACKAGE_PREFER_CONFIG
  RUNTIMES_x86_64-pc-windows-ntposix_CMAKE_BUILD_TYPE
  RUNTIMES_x86_64-pc-windows-ntposix_RUNTIMES_USE_LIBC
  RUNTIMES_x86_64-pc-windows-ntposix_LIBUNWIND_USE_COMPILER_RT
  RUNTIMES_x86_64-pc-windows-ntposix_LIBCXXABI_USE_COMPILER_RT
  RUNTIMES_x86_64-pc-windows-ntposix_LIBCXX_USE_COMPILER_RT
  RUNTIMES_x86_64-pc-windows-ntposix_LLVM_LIBC_FULL_BUILD
  RUNTIMES_x86_64-pc-windows-ntposix_LIBC_UNIT_TEST_LINK_FREESTANDING
  RUNTIMES_x86_64-pc-windows-ntposix_CMAKE_TOOLCHAIN_FILE
  BUILTINS_x86_64-pc-windows-ntposix_CMAKE_TOOLCHAIN_FILE
  CACHE STRING "")

# ---
# Stage 2-only settings (BOOTSTRAP_ prefix)
# ---
# These settings apply only to the native NTPOSIX build.

# Build LLVM/Clang using libc++. The resulting binaries depend on c++.dll
# and unwind.dll at runtime (distributed with the toolchain).
set(BOOTSTRAP_LLVM_ENABLE_LIBCXX ON CACHE BOOL "")

# Optimizations for stage2.
set(BOOTSTRAP_LLVM_ENABLE_LTO Thin CACHE STRING "")
# Stage 2 needs extra projects for full distribution.
set(BOOTSTRAP_LLVM_ENABLE_PROJECTS "clang;clang-tools-extra;lld" CACHE STRING "")
set(BOOTSTRAP_CLANG_PLUGIN_SUPPORT OFF CACHE BOOL "")
set(BOOTSTRAP_ENABLE_LINKER_BUILD_ID ON CACHE BOOL "")
set(BOOTSTRAP_ENABLE_X86_RELAX_RELOCATIONS ON CACHE BOOL "")
set(BOOTSTRAP_LLVM_ENABLE_PLUGINS OFF CACHE BOOL "")
set(BOOTSTRAP_LLVM_ENABLE_UNWIND_TABLES OFF CACHE BOOL "")
set(BOOTSTRAP_LLVM_USE_RELATIVE_PATHS_IN_FILES ON CACHE BOOL "")

# Pre-seed HAVE_LIBXML2 for stage 2. check_symbol_exists may fail during
# bootstrap when the CRT link environment is incomplete, but we build
# libxml2 ourselves so the library is known-good.
set(BOOTSTRAP_HAVE_LIBXML2 TRUE CACHE BOOL "")

# Distribution components for stage2.
include(${CMAKE_CURRENT_LIST_DIR}/WindowsItanium-distribution.cmake)
set(BOOTSTRAP_LLVM_TOOLCHAIN_TOOLS ${_WI_TOOLCHAIN_TOOLS} CACHE STRING "")
set(BOOTSTRAP_LLVM_DISTRIBUTION_COMPONENTS ${_WI_DISTRIBUTION_COMPONENTS} CACHE STRING "")

# ---
# Bootstrap to Stage 2
# ---

set(CLANG_ENABLE_BOOTSTRAP ON CACHE BOOL "")

# Build bootstrap cmake args for stage 2 (native NTPOSIX build).
# Stage 2 will build its own NTPOSIX ABI dependencies.
# Pass the stage 1 build directory so stage 2 can find runtimes and tools.
# Note: -C cache files run BEFORE -D options, so we must pass this via -D.
set(CLANG_BOOTSTRAP_CMAKE_ARGS
  -D_WI_PHASE1_BUILD_DIR=${CMAKE_BINARY_DIR}
  -DCMAKE_TOOLCHAIN_FILE=${RUNTIMES_x86_64-pc-windows-ntposix_CMAKE_TOOLCHAIN_FILE}
  -C ${CMAKE_CURRENT_LIST_DIR}/NTPOSIX-native.cmake
  CACHE STRING "")

# Note: LLVM_ENABLE_PROJECTS and LLVM_ENABLE_RUNTIMES are auto-passed via
# _BOOTSTRAP_DEFAULT_PASSTHROUGH, no need to set BOOTSTRAP_ versions.

# Tell bootstrap to use GNU-style driver (clang/clang++) instead of clang-cl.
set(BOOTSTRAP_LLVM_HOST_TRIPLE "x86_64-pc-windows-ntposix" CACHE STRING "")

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
