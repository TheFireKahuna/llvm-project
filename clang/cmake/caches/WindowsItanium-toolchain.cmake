# This file sets up a CMakeCache for a two-stage build of a toolchain that runs
# on Windows Itanium or NT-POSIX. It follows the environment's runtimes cache,
# whose first runtime target is the triple the toolchain runs on. From the
# llvm-project directory:
#
#   cmake -G Ninja \
#     -C clang/cmake/caches/WindowsItanium-runtimes.cmake \
#     -C clang/cmake/caches/WindowsItanium-toolchain.cmake \
#     -DCMAKE_PREFIX_PATH=<zlib, zstd and libxml2 for the Windows host> \
#     -DBOOTSTRAP_CMAKE_PREFIX_PATH=<zlib, zstd and libxml2 for the toolchain> \
#     llvm
#   ninja stage2-distribution
#
# The first stage, built on Windows with any compiler, builds clang, lld and the
# runtimes. The second stage is built by the first, with
# WindowsItanium-distribution.cmake, and links those runtimes.

set(CMAKE_BUILD_TYPE Release CACHE STRING "")

# lld-link merges the manifests that the driver embeds in executables itself
# only when built with libxml2.
set(LLVM_ENABLE_LIBXML2 FORCE_ON CACHE STRING "")
set(LLVM_USE_STATIC_LIBXML2 ON CACHE BOOL "")

list(GET LLVM_RUNTIME_TARGETS 0 host_triple)
if(host_triple MATCHES "-windows-ntposix")
  set(runtimes_cache ${CMAKE_CURRENT_LIST_DIR}/NTPOSIX-runtimes.cmake)
else()
  set(runtimes_cache ${CMAKE_CURRENT_LIST_DIR}/WindowsItanium-runtimes.cmake)
endif()
set(BOOTSTRAP_LLVM_HOST_TRIPLE "${host_triple}" CACHE STRING "")
set(BOOTSTRAP_LLVM_ENABLE_PROJECTS "clang;clang-tools-extra;lld"
    CACHE STRING "")

set(CLANG_ENABLE_BOOTSTRAP ON CACHE BOOL "")
set(CLANG_BOOTSTRAP_CMAKE_ARGS
  -C ${CMAKE_CURRENT_LIST_DIR}/WindowsItanium-distribution.cmake
  -C ${runtimes_cache}
  CACHE STRING "")
set(CLANG_BOOTSTRAP_TARGETS
  check-all
  check-clang
  check-lld
  check-llvm
  distribution
  install-distribution
  install-distribution-stripped
  CACHE STRING "")
