# NTPOSIX-native.cmake - Build native NTPOSIX LLVM toolchain
#
# Stage 2 of 2: Builds Clang/LLD/LLDB as native NTPOSIX binaries,
# linked against libc++. The resulting clang.exe itself uses the Itanium ABI.
#
# NTPOSIX is POSIX-on-NT: Itanium ABI, compiler-rt, libunwind, libc++,
# llvm-libc as the sole C runtime, lld-link, PE/COFF. No UCRT dependency.
#
# This phase is typically invoked automatically via bootstrap from
# NTPOSIX-runtimes.cmake. It can also be used standalone if you have:
#   1. A Clang that can target NTPOSIX
#   2. NTPOSIX runtimes (compiler-rt, libunwind, libc++abi, libc++, llvm-libc)
#      installed
#
# Prerequisites (for standalone use):
#   - Clang compiler with NTPOSIX support
#   - NTPOSIX runtimes installed (in compiler search paths or via
#     CMAKE_PREFIX_PATH pointing to install location)
#   - CMake 3.20+, Ninja, Python 3, Git
#
# Standalone build:
#   cmake -G Ninja -B build -C <path>/clang/cmake/caches/NTPOSIX-native.cmake \
#         -DCMAKE_C_COMPILER=<clang> -DCMAKE_CXX_COMPILER=<clang++> \
#         -DCMAKE_PREFIX_PATH=<runtimes-install-path> \
#         -DCMAKE_INSTALL_PREFIX=<install-path> <path>/llvm
#   ninja -C build distribution
#   ninja -C build install-distribution
#
# The native NTPOSIX toolchain will be in <install-path>.
#
# Note: Most settings are inherited from NTPOSIX-runtimes.cmake via
# CLANG_BOOTSTRAP_PASSTHROUGH and BOOTSTRAP_ prefixed variables. This file
# contains only stage2-specific configuration that cannot be passed through.

# Windows 10/11 with long paths enabled can tolerate much deeper object paths
# than CMake's conservative default.
set(CMAKE_OBJECT_PATH_MAX 32768 CACHE STRING "")

# ---
# Compiler Selection
# ---
# NTPOSIX uses GNU-style driver (clang/clang++), not clang-cl.

if(DEFINED _WI_PHASE1_BUILD_DIR)
  set(_WI_BOOTSTRAP_COMPILER_DIR "${_WI_PHASE1_BUILD_DIR}/bin")
endif()

if(NOT DEFINED CMAKE_C_COMPILER)
  if(DEFINED _WI_BOOTSTRAP_COMPILER_DIR)
    find_program(_WI_CLANG NAMES clang clang.exe
      HINTS "${_WI_BOOTSTRAP_COMPILER_DIR}" NO_DEFAULT_PATH)
  endif()
  if(NOT _WI_CLANG)
    find_program(_WI_CLANG NAMES clang clang.exe REQUIRED)
  endif()
  set(CMAKE_C_COMPILER "${_WI_CLANG}" CACHE FILEPATH "")
endif()

if(NOT DEFINED CMAKE_CXX_COMPILER)
  if(DEFINED _WI_BOOTSTRAP_COMPILER_DIR)
    find_program(_WI_CLANGXX NAMES clang++ clang++.exe
      HINTS "${_WI_BOOTSTRAP_COMPILER_DIR}" NO_DEFAULT_PATH)
  endif()
  if(NOT _WI_CLANGXX)
    find_program(_WI_CLANGXX NAMES clang++ clang++.exe REQUIRED)
  endif()
  set(CMAKE_CXX_COMPILER "${_WI_CLANGXX}" CACHE FILEPATH "")
endif()

# ---
# Build Dependencies with NTPOSIX ABI
# ---
# Build dependencies with NTPOSIX ABI using clang/clang++.
# These are used to build the native NTPOSIX toolchain itself.
#
# Note: Do NOT use vcpkg here - vcpkg packages are MSVC ABI, which is
# incompatible with NTPOSIX.

include(${CMAKE_CURRENT_LIST_DIR}/WindowsItanium-toolchain.cmake)

# Find compilers for dependency builds - use clang from stage 1 build.
# The stage 1 clang has the toolchain which automatically uses LLD.
#
# When bootstrapping from stage 1, _WI_PHASE1_BUILD_DIR is passed via -D option.
# For standalone use (not bootstrapping), fall back to CMAKE_C_COMPILER directory.
if(DEFINED _WI_PHASE1_BUILD_DIR)
  set(_WI_COMPILER_DIR "${_WI_PHASE1_BUILD_DIR}/bin")
  set(_WI_PHASE1_LIB "${_WI_PHASE1_BUILD_DIR}/lib")
else()
  get_filename_component(_WI_COMPILER_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
  get_filename_component(_WI_COMPILER_DIR "${_WI_COMPILER_DIR}" ABSOLUTE)
  get_filename_component(_WI_PHASE1_ROOT "${_WI_COMPILER_DIR}" DIRECTORY)
  set(_WI_PHASE1_LIB "${_WI_PHASE1_ROOT}/lib")
endif()
unset(_WI_DEP_CC CACHE)
unset(_WI_DEP_CXX CACHE)
find_program(_WI_DEP_CC NAMES clang HINTS "${_WI_COMPILER_DIR}" NO_DEFAULT_PATH REQUIRED)
find_program(_WI_DEP_CXX NAMES clang++ HINTS "${_WI_COMPILER_DIR}" NO_DEFAULT_PATH REQUIRED)

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
  ABI_SUFFIX "-ntposix"
  TARGET "x86_64-pc-windows-ntposix"
)

# Add dependency install directories to CMAKE_PREFIX_PATH so find_package can
# locate them in sub-builds (runtimes external project).
set(CMAKE_PREFIX_PATH "${ZLIB_ROOT};${zstd_ROOT};${LibXml2_ROOT}" CACHE PATH "")

# Pass CMAKE_PREFIX_PATH and *_ROOT variables to runtimes external project.
# Stage2's top-level configure has already identified the native NTPOSIX
# compiler ABI. Propagate those results into nested external projects so they
# don't re-run compiler ABI probes with the freshly built driver.
#
# These bootstrapped NTPOSIX toolchains always use libc++, never libstdc++.
# Seed the known-negative libstdc++ checks so nested runtimes configure does
# not spin up redundant try-compiles under -nostdinc++ / -nostdlib++.
set(LLVM_USES_LIBSTDCXX OFF CACHE BOOL "" FORCE)
set(LLVM_DEFAULT_TO_GLIBCXX_USE_CXX11_ABI OFF CACHE BOOL "" FORCE)
set(RUNTIMES_X86_64_PC_WINDOWS_NTPOSIX_CMAKE_ARGS
  -DLLVM_USES_LIBSTDCXX=OFF
  -DLLVM_DEFAULT_TO_GLIBCXX_USE_CXX11_ABI=OFF
  CACHE STRING "" FORCE)

# The freshly built native NTPOSIX toolchain can deadlock when CMake drives
# try_compile() through a full executable link in nested runtimes configure.
# Compile-only probes are sufficient for these configure-time feature checks and
# match how other cross/runtime caches avoid fragile bootstrap link steps.
set(RUNTIMES_x86_64-pc-windows-ntposix_CMAKE_TRY_COMPILE_TARGET_TYPE
  STATIC_LIBRARY CACHE STRING "" FORCE)

set(LLVM_EXTERNAL_PROJECT_PASSTHROUGH
  CMAKE_PREFIX_PATH
  CMAKE_C_ABI_COMPILED
  CMAKE_CXX_ABI_COMPILED
  CMAKE_C_COMPILER_ABI
  CMAKE_CXX_COMPILER_ABI
  CMAKE_C_COMPILER_ARCHITECTURE_ID
  CMAKE_CXX_COMPILER_ARCHITECTURE_ID
  CMAKE_C_SIZEOF_DATA_PTR
  CMAKE_CXX_SIZEOF_DATA_PTR
  CMAKE_C_BYTE_ORDER
  CMAKE_CXX_BYTE_ORDER
  CMAKE_SIZEOF_VOID_P
  LLVM_USES_LIBSTDCXX
  LLVM_DEFAULT_TO_GLIBCXX_USE_CXX11_ABI
  ZLIB_ROOT
  zstd_ROOT
  LibXml2_ROOT
  CACHE STRING "" FORCE)

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
# Native NTPOSIX Build Configuration
# ---
# These settings are specific to the native build and cannot be passed through.

# Target configuration - this clang.exe will BE an NTPOSIX binary.
set(LLVM_DEFAULT_TARGET_TRIPLE "x86_64-pc-windows-ntposix" CACHE STRING "")
set(LLVM_HOST_TRIPLE "x86_64-pc-windows-ntposix" CACHE STRING "")

# Tell CMake to compile for NTPOSIX target.
set(CMAKE_C_COMPILER_TARGET "x86_64-pc-windows-ntposix" CACHE STRING "")
set(CMAKE_CXX_COMPILER_TARGET "x86_64-pc-windows-ntposix" CACHE STRING "")
set(CMAKE_ASM_COMPILER_TARGET "x86_64-pc-windows-ntposix" CACHE STRING "")

# ---
# Compiler Flags
# ---

# Suppress warnings for Microsoft extensions in LLVM headers.
set(CMAKE_C_FLAGS "-Wno-language-extension-token -Wno-microsoft-enum-value" CACHE STRING "")
set(CMAKE_CXX_FLAGS "-Wno-language-extension-token -Wno-microsoft-enum-value" CACHE STRING "")

# Add library path and libraries for stage 1 runtimes.
# CMake's try_compile tests need to find and link c++.lib and unwind.lib.
# Use -L (GNU-style) which the toolchain converts to -libpath:.
# Also explicitly add libraries since CMake may pass -nostdlib which suppresses
# the toolchain's automatic -defaultlib: additions.
if(EXISTS "${_WI_PHASE1_LIB}/c++.lib")
  set(CMAKE_EXE_LINKER_FLAGS "-L\"${_WI_PHASE1_LIB}\" -lc++ -lunwind" CACHE STRING "" FORCE)
  set(CMAKE_SHARED_LINKER_FLAGS "-L\"${_WI_PHASE1_LIB}\" -lc++ -lunwind" CACHE STRING "" FORCE)
else()
  message(WARNING "Stage 1 runtimes not found at ${_WI_PHASE1_LIB}/c++.lib; "
    "CMake try_compile may fail. Set CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY as workaround.")
endif()

# ---
# Bootstrap Termination
# ---

# This is the final stage - no further bootstrap.
set(CLANG_ENABLE_BOOTSTRAP OFF CACHE BOOL "")

# ---
# Standalone Mode Defaults
# ---
# These are only used when running standalone (not via bootstrap).
# When bootstrapping, these are overridden by values passed from stage 1.

if(NOT DEFINED PACKAGE_VENDOR)
  set(PACKAGE_VENDOR "NTPOSIX" CACHE STRING "")
endif()

if(NOT DEFINED LLVM_TARGETS_TO_BUILD)
  set(LLVM_TARGETS_TO_BUILD "X86" CACHE STRING "")
endif()

if(NOT DEFINED LLVM_ENABLE_PROJECTS)
  set(LLVM_ENABLE_PROJECTS "clang;clang-tools-extra;lld" CACHE STRING "")
endif()

if(NOT DEFINED LLVM_ENABLE_RUNTIMES)
  set(LLVM_ENABLE_RUNTIMES "compiler-rt;libunwind;libcxxabi;libcxx;libc" CACHE STRING "")
endif()

# LLVM libc is the sole C runtime for NTPOSIX - enable full build.
set(LLVM_LIBC_FULL_BUILD ON CACHE BOOL "")

# NTPOSIX: the compiler driver injects our c.lib as the default libc, so the
# test link line never sees a foreign libc. Link unit tests as freestanding
# (same recipe as hermetic tests) so __llvm_libc_* public symbols resolve
# uniquely from the libc __internal__ objects instead of colliding with
# c.dll's exports.
set(LIBC_UNIT_TEST_LINK_FREESTANDING ON CACHE BOOL "")

if(NOT DEFINED LLVM_RUNTIME_TARGETS)
  set(LLVM_RUNTIME_TARGETS "x86_64-pc-windows-ntposix" CACHE STRING "")
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

if(NOT DEFINED RUNTIMES_x86_64-pc-windows-ntposix_CMAKE_BUILD_TYPE)
  set(RUNTIMES_x86_64-pc-windows-ntposix_CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "")
endif()

# Standalone distribution components (used when not bootstrapping).
if(NOT DEFINED LLVM_DISTRIBUTION_COMPONENTS)
  include(${CMAKE_CURRENT_LIST_DIR}/WindowsItanium-distribution.cmake)
  set(LLVM_TOOLCHAIN_TOOLS ${_WI_TOOLCHAIN_TOOLS} CACHE STRING "")
  set(LLVM_DISTRIBUTION_COMPONENTS ${_WI_DISTRIBUTION_COMPONENTS} CACHE STRING "")
endif()
