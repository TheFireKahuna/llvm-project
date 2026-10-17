# This file sets up a CMakeCache for a toolchain that runs on Windows Itanium or
# NT-POSIX, built with a clang that targets it and links its runtimes. It
# precedes the environment's runtimes cache, and is the second stage of
# WindowsItanium-toolchain.cmake.
# From the llvm-project directory:
#
#   cmake -G Ninja \
#     -C clang/cmake/caches/WindowsItanium-distribution.cmake \
#     -C clang/cmake/caches/WindowsItanium-runtimes.cmake \
#     -DCMAKE_PREFIX_PATH=<zlib, zstd and libxml2 for the toolchain> \
#     llvm
#   ninja distribution

# CMake would take the GNU-style driver for MinGW's.
cmake_path(APPEND CMAKE_CURRENT_LIST_DIR ../../../llvm/cmake/platforms
           WindowsItaniumToolchain.cmake OUTPUT_VARIABLE toolchain_file)
set(CMAKE_TOOLCHAIN_FILE ${toolchain_file} CACHE FILEPATH "")
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(LLVM_ENABLE_PROJECTS "clang;clang-tools-extra;lld" CACHE STRING "")

# lld-link merges the manifests that the driver embeds in executables itself
# only when built with libxml2.
set(LLVM_ENABLE_LIBXML2 FORCE_ON CACHE STRING "")
set(LLVM_USE_STATIC_LIBXML2 ON CACHE BOOL "")

set(LLVM_INSTALL_TOOLCHAIN_ONLY ON CACHE BOOL "")
set(LLVM_TOOLCHAIN_TOOLS
  llvm-ar
  llvm-cov
  llvm-cxxfilt
  llvm-dlltool
  llvm-dwarfdump
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
  llvm-readobj
  llvm-size
  llvm-strings
  llvm-strip
  llvm-symbolizer
  llvm-undname
  CACHE STRING "")
set(LLVM_DISTRIBUTION_COMPONENTS
  clang
  clang-format
  clang-resource-headers
  clang-scan-deps
  clang-tidy
  clangd
  lld
  LTO
  builtins
  runtimes
  ${LLVM_TOOLCHAIN_TOOLS}
  CACHE STRING "")
