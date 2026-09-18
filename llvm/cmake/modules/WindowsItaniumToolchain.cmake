# WindowsItaniumToolchain.cmake - Pre-project setup for Windows Itanium
#
# Include this BEFORE project() to configure Windows Itanium builds.
# Sets CMAKE_USER_MAKE_RULES_OVERRIDE and CMAKE_RC_COMPILER_INIT to prevent
# CMake from using MinGW-style platform rules and finding windres.

if(CMAKE_C_COMPILER_TARGET MATCHES "windows-(itanium|ntposix)" OR
   CMAKE_CXX_COMPILER_TARGET MATCHES "windows-(itanium|ntposix)" OR
   LLVM_HOST_TRIPLE MATCHES "windows-(itanium|ntposix)" OR
   LLVM_RUNTIMES_TARGET MATCHES "windows-(itanium|ntposix)")

  # Override Windows-GNU.cmake rules with MSVC-style linking for lld-link.
  if(NOT CMAKE_USER_MAKE_RULES_OVERRIDE)
    set(CMAKE_USER_MAKE_RULES_OVERRIDE
        "${CMAKE_CURRENT_LIST_DIR}/WindowsItaniumRules.cmake")
  endif()

  # Set RC compiler before project() to prevent Windows-GNU.cmake from finding
  # windres. Prefer llvm-rc from the selected toolchain; fall back to rc.exe
  # from the VS/Windows SDK environment when llvm-rc is not installed yet.
  if(NOT CMAKE_RC_COMPILER_INIT AND NOT CMAKE_RC_COMPILER)
    unset(_wi_compiler_dir)
    if(CMAKE_C_COMPILER)
      get_filename_component(_wi_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    endif()

    set(_wi_rc_hints)
    if(_wi_compiler_dir)
      list(APPEND _wi_rc_hints "${_wi_compiler_dir}")
    endif()

    find_program(_wi_rc_compiler NAMES llvm-rc rc HINTS ${_wi_rc_hints}
                 NO_DEFAULT_PATH)
    if(NOT _wi_rc_compiler)
      find_program(_wi_rc_compiler NAMES llvm-rc rc)
    endif()
    if(_wi_rc_compiler)
      set(CMAKE_RC_COMPILER_INIT "${_wi_rc_compiler}")
      set(CMAKE_RC_COMPILER "${_wi_rc_compiler}")
    endif()

    unset(_wi_rc_compiler)
    unset(_wi_rc_hints)
    unset(_wi_compiler_dir)
  endif()
endif()
