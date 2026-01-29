# WindowsItaniumDependencies.cmake - Helper functions for building dependencies
#
# Provides a reusable function to clone, build, and install dependencies
# for both MSVC ABI (Phase 2) and Windows Itanium ABI (Phase 3).

#===------------------------------------------------------------------------===#
# Helper function to build a dependency library
#===------------------------------------------------------------------------===#
#
# Arguments:
#   NAME        - Dependency name (used for directory naming and messages)
#   GIT_URL     - Git repository URL
#   GIT_TAG     - Git tag or branch to checkout
#   CMAKE_SUBDIR - Subdirectory containing CMakeLists.txt (empty for root)
#   INSTALL_VAR - CMake variable name to set with install path (e.g., ZLIB_ROOT)
#   CHECK_FILES - Semicolon-separated list of files to check for (skip build if any exist)
#   OPTIONS     - Additional CMake options for configure
#
# Required variables (must be set before calling):
#   _WI_DEP_COMPILER    - C compiler to use
#   _WI_DEP_CXX_COMPILER - C++ compiler to use (optional, defaults to _WI_DEP_COMPILER)
#   _WI_DEP_BUILD_DIR   - Base directory for builds
#   _WI_DEP_ABI_SUFFIX  - Suffix for install dir (e.g., "-msvc" or "-wi")
#   _WI_DEP_TARGET      - Target triple (empty for native/MSVC)
#
function(wi_build_dependency)
  cmake_parse_arguments(
    DEP
    ""
    "NAME;GIT_URL;GIT_TAG;CMAKE_SUBDIR;INSTALL_VAR"
    "CHECK_FILES;OPTIONS"
    ${ARGN}
  )

  set(_install_dir "${_WI_DEP_BUILD_DIR}/${DEP_NAME}${_WI_DEP_ABI_SUFFIX}")

  # Check if already built
  set(_already_built FALSE)
  foreach(_check_file IN LISTS DEP_CHECK_FILES)
    if(EXISTS "${_install_dir}/${_check_file}")
      set(_already_built TRUE)
      break()
    endif()
  endforeach()

  if(_already_built)
    message(STATUS "${DEP_NAME}: Found existing installation at ${_install_dir}")
  else()
    message(STATUS "=== Building ${DEP_NAME} (${_WI_DEP_ABI_SUFFIX}) ===")

    set(_src_dir "${_WI_DEP_BUILD_DIR}/${DEP_NAME}-src")
    set(_build_dir "${_WI_DEP_BUILD_DIR}/${DEP_NAME}-build${_WI_DEP_ABI_SUFFIX}")

    # Clone if needed
    if(DEP_CMAKE_SUBDIR)
      set(_cmake_check "${_src_dir}/${DEP_CMAKE_SUBDIR}/CMakeLists.txt")
    else()
      set(_cmake_check "${_src_dir}/CMakeLists.txt")
    endif()

    if(NOT EXISTS "${_cmake_check}")
      message(STATUS "Cloning ${DEP_NAME} ${DEP_GIT_TAG}...")
      execute_process(
        COMMAND git -c advice.detachedHead=false clone --quiet --depth 1
                --branch "${DEP_GIT_TAG}" "${DEP_GIT_URL}" "${_src_dir}"
        RESULT_VARIABLE _result
      )
      if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Failed to clone ${DEP_NAME}")
      endif()
    endif()

    # Determine source directory for CMake
    if(DEP_CMAKE_SUBDIR)
      set(_cmake_src "${_src_dir}/${DEP_CMAKE_SUBDIR}")
    else()
      set(_cmake_src "${_src_dir}")
    endif()

    # Build compiler flags
    set(_c_flags "-w")
    set(_cxx_flags "-w")
    if(_WI_DEP_TARGET)
      string(APPEND _c_flags " --target=${_WI_DEP_TARGET}")
      string(APPEND _cxx_flags " --target=${_WI_DEP_TARGET}")
    endif()

    # Build configure command
    set(_configure_cmd
      ${CMAKE_COMMAND}
      -G Ninja
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_INSTALL_PREFIX=${_install_dir}
      -DCMAKE_C_COMPILER=${_WI_DEP_COMPILER}
      "-DCMAKE_C_FLAGS=${_c_flags}"
    )

    # Add C++ compiler if specified
    if(_WI_DEP_CXX_COMPILER)
      list(APPEND _configure_cmd
        -DCMAKE_CXX_COMPILER=${_WI_DEP_CXX_COMPILER}
        "-DCMAKE_CXX_FLAGS=${_cxx_flags}"
      )
    endif()

    # Add user options
    foreach(_opt IN LISTS DEP_OPTIONS)
      list(APPEND _configure_cmd "${_opt}")
    endforeach()

    # Add source and build dirs
    list(APPEND _configure_cmd -S "${_cmake_src}" -B "${_build_dir}")

    # Configure
    message(STATUS "Configuring ${DEP_NAME}...")
    execute_process(
      COMMAND ${_configure_cmd}
      RESULT_VARIABLE _result
    )
    if(NOT _result EQUAL 0)
      message(FATAL_ERROR "Failed to configure ${DEP_NAME}")
    endif()

    # Build
    message(STATUS "Building ${DEP_NAME}...")
    execute_process(
      COMMAND ${CMAKE_COMMAND} --build "${_build_dir}"
      RESULT_VARIABLE _result
    )
    if(NOT _result EQUAL 0)
      message(FATAL_ERROR "Failed to build ${DEP_NAME}")
    endif()

    # Install
    message(STATUS "Installing ${DEP_NAME}...")
    execute_process(
      COMMAND ${CMAKE_COMMAND} --install "${_build_dir}"
      RESULT_VARIABLE _result
    )
    if(NOT _result EQUAL 0)
      message(FATAL_ERROR "Failed to install ${DEP_NAME}")
    endif()

    message(STATUS "=== ${DEP_NAME} built successfully ===")
  endif()

  # Export the install path
  set(${DEP_INSTALL_VAR} "${_install_dir}" CACHE PATH "")
  # Also set in parent scope for immediate use
  set(${DEP_INSTALL_VAR} "${_install_dir}" PARENT_SCOPE)
endfunction()

#===------------------------------------------------------------------------===#
# Build all Windows Itanium dependencies
#===------------------------------------------------------------------------===#
#
# Arguments:
#   COMPILER     - C compiler path
#   CXX_COMPILER - C++ compiler path (optional)
#   BUILD_DIR    - Base directory for source/build/install
#   ABI_SUFFIX   - Suffix like "-msvc" or "-wi"
#   TARGET       - Target triple (empty for native)
#
function(wi_build_all_dependencies)
  cmake_parse_arguments(
    ARG
    ""
    "COMPILER;CXX_COMPILER;BUILD_DIR;ABI_SUFFIX;TARGET"
    ""
    ${ARGN}
  )

  # Set up context for wi_build_dependency
  set(_WI_DEP_COMPILER "${ARG_COMPILER}")
  set(_WI_DEP_CXX_COMPILER "${ARG_CXX_COMPILER}")
  set(_WI_DEP_BUILD_DIR "${ARG_BUILD_DIR}")
  set(_WI_DEP_ABI_SUFFIX "${ARG_ABI_SUFFIX}")
  set(_WI_DEP_TARGET "${ARG_TARGET}")

  # zlib - build static only to avoid runtime DLL dependency
  wi_build_dependency(
    NAME zlib
    GIT_URL https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
    INSTALL_VAR ZLIB_ROOT
    CHECK_FILES "lib/zlibstatic.lib"
    OPTIONS
      -DBUILD_SHARED_LIBS=OFF
      -DZLIB_BUILD_EXAMPLES=OFF
  )
  # Propagate to parent
  set(ZLIB_ROOT "${ZLIB_ROOT}" PARENT_SCOPE)

  # zstd
  wi_build_dependency(
    NAME zstd
    GIT_URL https://github.com/facebook/zstd.git
    GIT_TAG v1.5.7
    CMAKE_SUBDIR build/cmake
    INSTALL_VAR zstd_ROOT
    CHECK_FILES "lib/zstd_static.lib;lib/zstd.lib"
    OPTIONS
      -DZSTD_BUILD_PROGRAMS=OFF
      -DZSTD_BUILD_TESTS=OFF
      -DZSTD_BUILD_SHARED=OFF
      -DZSTD_BUILD_STATIC=ON
  )
  set(zstd_ROOT "${zstd_ROOT}" PARENT_SCOPE)

  # libxml2
  wi_build_dependency(
    NAME libxml2
    GIT_URL https://github.com/GNOME/libxml2.git
    GIT_TAG v2.14.0
    INSTALL_VAR LibXml2_ROOT
    CHECK_FILES "lib/libxml2.lib;lib/xml2.lib"
    OPTIONS
      -DBUILD_SHARED_LIBS=OFF
      -DLIBXML2_WITH_ICONV=OFF
      -DLIBXML2_WITH_LZMA=OFF
      -DLIBXML2_WITH_PYTHON=OFF
      -DLIBXML2_WITH_ZLIB=OFF
      -DLIBXML2_WITH_TESTS=OFF
      -DLIBXML2_WITH_PROGRAMS=OFF
      -DLIBXML2_WITH_HTTP=OFF
      -DLIBXML2_WITH_FTP=OFF
  )
  set(LibXml2_ROOT "${LibXml2_ROOT}" PARENT_SCOPE)
endfunction()
