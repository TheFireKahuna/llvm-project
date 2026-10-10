# Toolchain file for building with a clang driver that targets
# *-windows-itanium or *-windows-ntposix.
#
# Usage:
# cmake -G Ninja
#    -DCMAKE_TOOLCHAIN_FILE=/path/to/this/file
#    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#    -DCMAKE_C_COMPILER_TARGET=x86_64-unknown-windows-itanium
#    -DCMAKE_CXX_COMPILER_TARGET=x86_64-unknown-windows-itanium
#
# The triple may also come from LLVM_RUNTIMES_TARGET or LLVM_HOST_TRIPLE.
#
# CMake identifies these compilers as Clang without an MSVC simulation, since
# they do not define _MSC_VER, and so gives them the MinGW platform rules. Their
# images are linked by lld-link, as MSVC's are, so WindowsItaniumRules.cmake
# replaces the parts of those rules that differ.

include(
  ${CMAKE_CURRENT_LIST_DIR}/../../../cmake/Modules/DetectWindowsItanium.cmake)
if(WIN32_ITANIUM OR WIN32_NTPOSIX)
  set(CMAKE_USER_MAKE_RULES_OVERRIDE
      ${CMAKE_CURRENT_LIST_DIR}/WindowsItaniumRules.cmake)

  # Choose llvm-rc before the platform rules would pick windres. CMake's own
  # rules then preprocess resource scripts with clang. Prefer the one next to
  # the compiler, so that a toolchain being bootstrapped uses its own tools.
  if(NOT CMAKE_RC_COMPILER)
    set(_rc_hints)
    if(CMAKE_C_COMPILER)
      get_filename_component(_rc_hints "${CMAKE_C_COMPILER}" DIRECTORY)
    endif()
    find_program(_rc_compiler NAMES llvm-rc HINTS ${_rc_hints} NO_DEFAULT_PATH)
    if(NOT _rc_compiler)
      find_program(_rc_compiler NAMES llvm-rc)
    endif()
    if(_rc_compiler)
      set(CMAKE_RC_COMPILER_INIT "${_rc_compiler}")
    endif()
    unset(_rc_compiler CACHE)
    unset(_rc_hints)
  endif()
endif()
