# WindowsItanium-distribution.cmake
#
# Shared toolchain tools and distribution component lists used by both
# Windows Itanium and NT-POSIX cache files.  Include this file before
# setting LLVM_DISTRIBUTION_COMPONENTS to get the common baseline.
#
# After including, each cache file may append target-specific entries
# (e.g. lldb) to _WI_DISTRIBUTION_COMPONENTS before writing the final
# LLVM_DISTRIBUTION_COMPONENTS / BOOTSTRAP_LLVM_DISTRIBUTION_COMPONENTS.

set(_WI_TOOLCHAIN_TOOLS
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
)

set(_WI_DISTRIBUTION_COMPONENTS
  clang
  clang-format
  clang-resource-headers
  builtins
  clang-tidy
  clangd
  lld
  LTO
  runtimes
  ${_WI_TOOLCHAIN_TOOLS}
)
