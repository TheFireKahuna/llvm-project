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
  # windows_itanium_joint_link(), which declares a joint link.
  include(${CMAKE_CURRENT_LIST_DIR}/WindowsItaniumJointLink.cmake)

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

  # CMake's MASM rules name Visual C++'s assembler from an MSVC compiler's
  # architecture, which this compiler does not report, so they would pick the
  # 32-bit ml. An x86-64 target assembles with llvm-ml64, again preferring the
  # one next to the compiler; LLVM has no MASM assembler for ARM64.
  if(NOT CMAKE_ASM_MASM_COMPILER AND
     WIN32_ITANIUM_TRIPLE MATCHES "^(x86_64|amd64)-")
    set(_ml_hints)
    if(CMAKE_C_COMPILER)
      get_filename_component(_ml_hints "${CMAKE_C_COMPILER}" DIRECTORY)
    endif()
    find_program(_ml_compiler NAMES llvm-ml64 HINTS ${_ml_hints}
                 NO_DEFAULT_PATH)
    if(NOT _ml_compiler)
      find_program(_ml_compiler NAMES llvm-ml64)
    endif()
    if(_ml_compiler)
      set(CMAKE_ASM_MASM_COMPILER "${_ml_compiler}")
    endif()
    unset(_ml_compiler CACHE)
    unset(_ml_hints)
  endif()
endif()
