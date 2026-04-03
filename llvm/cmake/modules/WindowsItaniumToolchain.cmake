# WindowsItaniumToolchain.cmake - Pre-project setup for Windows Itanium
#
# Include this BEFORE project() to configure Windows Itanium builds.
# Sets CMAKE_USER_MAKE_RULES_OVERRIDE and CMAKE_RC_COMPILER_INIT to prevent
# CMake from using MinGW-style platform rules and finding windres.

if(CMAKE_C_COMPILER_TARGET MATCHES "windows-itanium" OR
   CMAKE_CXX_COMPILER_TARGET MATCHES "windows-itanium" OR
   LLVM_HOST_TRIPLE MATCHES "windows-itanium" OR
   LLVM_RUNTIMES_TARGET MATCHES "windows-itanium")

  # Override Windows-GNU.cmake rules with MSVC-style linking for lld-link.
  if(NOT CMAKE_USER_MAKE_RULES_OVERRIDE)
    set(CMAKE_USER_MAKE_RULES_OVERRIDE
        "${CMAKE_CURRENT_LIST_DIR}/WindowsItaniumRules.cmake")
  endif()

  # Set RC compiler before project() to prevent Windows-GNU.cmake from finding windres.
  if(CMAKE_C_COMPILER AND NOT CMAKE_RC_COMPILER_INIT)
    get_filename_component(_wi_compiler_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    find_program(_wi_llvm_rc NAMES llvm-rc HINTS "${_wi_compiler_dir}" NO_DEFAULT_PATH)
    if(NOT _wi_llvm_rc)
      find_program(_wi_llvm_rc NAMES llvm-rc)
    endif()
    if(_wi_llvm_rc)
      set(CMAKE_RC_COMPILER_INIT "${_wi_llvm_rc}")
      set(CMAKE_RC_COMPILER "${_wi_llvm_rc}")
    endif()
    unset(_wi_llvm_rc)
    unset(_wi_compiler_dir)
  endif()
endif()
