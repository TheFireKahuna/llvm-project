# This file builds zlib, zstd and libxml2 while the cache is loaded, and points
# the LLVM build at them. It belongs to this fork's Windows Itanium and NT-POSIX
# caches and is not meant for upstream, whose caches take dependencies from
# CMAKE_PREFIX_PATH instead of fetching them.
#
# Load it after the other caches. For the two-stage toolchain, from the
# llvm-project directory:
#
#   cmake -G Ninja \
#     -C clang/cmake/caches/WindowsItanium-runtimes.cmake \
#     -C clang/cmake/caches/WindowsItanium-toolchain.cmake \
#     -C clang/cmake/caches/WindowsItanium-dependencies.cmake \
#     llvm
#   ninja stage2-distribution
#
# The first stage's dependencies are built with the host's default compiler, or
# with CMAKE_C_COMPILER when it is given before the -C options. The second
# stage's are built, when it is configured, by the first stage's clang for the
# second stage's triple. A build with a clang that already targets Windows
# Itanium or NT-POSIX names it before the -C options:
#
#   -DWINDOWS_ITANIUM_DEPENDENCIES_TOOLS=<directory of clang.exe>
#   -DWINDOWS_ITANIUM_DEPENDENCIES_TARGET=x86_64-unknown-windows-itanium
#
# The sources are cloned once, into WINDOWS_ITANIUM_DEPENDENCIES_SOURCE_DIR,
# which may instead be filled beforehand for a build without network access.
# Each library is built again only when its version below changes.

set(WINDOWS_ITANIUM_ZLIB_VERSION 1.3.1)
set(WINDOWS_ITANIUM_ZSTD_VERSION 1.5.7)
set(WINDOWS_ITANIUM_LIBXML2_VERSION 2.14.6)

set(WINDOWS_ITANIUM_DEPENDENCIES_SOURCE_DIR
  "${CMAKE_BINARY_DIR}/windows-itanium-deps/src" CACHE PATH
  "Sources of zlib, zstd and libxml2 for the Windows Itanium caches")

if(WINDOWS_ITANIUM_DEPENDENCIES_TOOLS)
  if(NOT WINDOWS_ITANIUM_DEPENDENCIES_TARGET)
    message(FATAL_ERROR "WINDOWS_ITANIUM_DEPENDENCIES_TOOLS needs "
                        "WINDOWS_ITANIUM_DEPENDENCIES_TARGET")
  endif()
  set(deps_install_dir
    "${CMAKE_BINARY_DIR}/windows-itanium-deps/${WINDOWS_ITANIUM_DEPENDENCIES_TARGET}")
else()
  set(deps_install_dir "${CMAKE_BINARY_DIR}/windows-itanium-deps/host")
endif()

# The arguments that every dependency's configuration shares.
find_program(deps_ninja NAMES ninja REQUIRED)
set(deps_common_args
  -G Ninja
  -DCMAKE_MAKE_PROGRAM=${deps_ninja}
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX=${deps_install_dir}
  -DBUILD_SHARED_LIBS=OFF
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
if(WINDOWS_ITANIUM_DEPENDENCIES_TOOLS)
  set(target "${WINDOWS_ITANIUM_DEPENDENCIES_TARGET}")
  set(tools "${WINDOWS_ITANIUM_DEPENDENCIES_TOOLS}")
  find_program(deps_cc NAMES clang HINTS "${tools}" NO_DEFAULT_PATH REQUIRED)
  find_program(deps_cxx NAMES clang++ HINTS "${tools}" NO_DEFAULT_PATH REQUIRED)
  find_program(deps_ar NAMES llvm-ar HINTS "${tools}" NO_DEFAULT_PATH REQUIRED)
  find_program(deps_ranlib NAMES llvm-ranlib HINTS "${tools}" NO_DEFAULT_PATH
               REQUIRED)
  cmake_path(APPEND CMAKE_CURRENT_LIST_DIR ../../../llvm/cmake/platforms
             WindowsItaniumToolchain.cmake OUTPUT_VARIABLE deps_toolchain)
  list(APPEND deps_common_args
    -DCMAKE_TOOLCHAIN_FILE=${deps_toolchain}
    -DCMAKE_C_COMPILER=${deps_cc}
    -DCMAKE_C_COMPILER_TARGET=${target}
    -DCMAKE_CXX_COMPILER=${deps_cxx}
    -DCMAKE_CXX_COMPILER_TARGET=${target}
    -DCMAKE_AR=${deps_ar}
    -DCMAKE_RANLIB=${deps_ranlib})
  if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND deps_common_args -DCMAKE_SYSTEM_NAME=Windows)
  endif()
  # NT-POSIX's C library is POSIX's, so the dependencies take their POSIX
  # paths, not the ones written for the Microsoft CRT.
  if(target MATCHES "-windows-ntposix")
    list(APPEND deps_common_args "-DCMAKE_C_FLAGS=-U_WIN32 -UWIN32"
         "-DCMAKE_CXX_FLAGS=-U_WIN32 -UWIN32")
  endif()
  unset(deps_cc CACHE)
  unset(deps_cxx CACHE)
  unset(deps_ar CACHE)
  unset(deps_ranlib CACHE)
else()
  if(CMAKE_C_COMPILER)
    list(APPEND deps_common_args -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
  endif()
  if(CMAKE_CXX_COMPILER)
    list(APPEND deps_common_args -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
  endif()
endif()
unset(deps_ninja CACHE)

# Clones, configures, builds and installs one dependency, unless its stamp
# already names this version. SOURCE_SUBDIR is where its CMakeLists.txt is,
# TARGET the one target to build instead of all, and the rest are options.
function(windows_itanium_build_dependency name repository version)
  cmake_parse_arguments(ARG "" "SOURCE_SUBDIR;TARGET" "OPTIONS" ${ARGN})
  set(source_dir "${WINDOWS_ITANIUM_DEPENDENCIES_SOURCE_DIR}/${name}-${version}")
  get_filename_component(variant "${deps_install_dir}" NAME)
  set(build_dir "${CMAKE_BINARY_DIR}/windows-itanium-deps/build/${name}-${variant}")
  set(stamp "${deps_install_dir}/${name}.stamp")
  if(EXISTS "${stamp}")
    file(READ "${stamp}" built_version)
    if(built_version STREQUAL version)
      return()
    endif()
  endif()

  if(NOT EXISTS "${source_dir}")
    message(STATUS "Cloning ${name} ${version}")
    execute_process(
      COMMAND git -c advice.detachedHead=false clone --quiet --depth 1
              --branch v${version} ${repository} ${source_dir}
      RESULT_VARIABLE result)
    if(result)
      message(FATAL_ERROR "Could not clone ${name} from ${repository}")
    endif()
  endif()

  # A static libxml2 needs no version resource, which a static library cannot
  # carry to the images that link it.
  if(name STREQUAL "libxml2")
    file(READ "${source_dir}/CMakeLists.txt" contents)
    string(REPLACE
      "if(WIN32)\n    list(APPEND LIBXML2_SRCS win32/libxml2.rc)"
      "if(WIN32 AND BUILD_SHARED_LIBS)\n    list(APPEND LIBXML2_SRCS win32/libxml2.rc)"
      patched "${contents}")
    if(NOT patched STREQUAL contents)
      file(WRITE "${source_dir}/CMakeLists.txt" "${patched}")
    endif()
  endif()

  message(STATUS "Building ${name} ${version} for ${variant}")
  file(REMOVE_RECURSE "${build_dir}")
  file(MAKE_DIRECTORY "${build_dir}")
  set(steps configure build)
  set(configure_command ${CMAKE_COMMAND} ${deps_common_args} ${ARG_OPTIONS}
      -S ${source_dir}/${ARG_SOURCE_SUBDIR} -B ${build_dir})
  set(build_command ${CMAKE_COMMAND} --build ${build_dir})
  if(ARG_TARGET)
    list(APPEND build_command --target ${ARG_TARGET})
  else()
    list(APPEND steps install)
    set(install_command ${CMAKE_COMMAND} --install ${build_dir})
  endif()
  foreach(step IN LISTS steps)
    execute_process(COMMAND ${${step}_command}
      OUTPUT_FILE ${build_dir}/${step}.log ERROR_FILE ${build_dir}/${step}.log
      RESULT_VARIABLE result)
    if(result)
      message(FATAL_ERROR
        "Could not ${step} ${name}; see ${build_dir}/${step}.log")
    endif()
  endforeach()

  # zlib builds its shared library whenever it installs, so only its static
  # library is built, and installed here.
  if(ARG_TARGET)
    file(GLOB library "${build_dir}/*zlibstatic*.lib")
    file(COPY ${library} DESTINATION "${deps_install_dir}/lib")
    file(COPY "${source_dir}/zlib.h" "${build_dir}/zconf.h"
         DESTINATION "${deps_install_dir}/include")
  endif()
  file(WRITE "${stamp}" "${version}")
endfunction()

windows_itanium_build_dependency(zlib https://github.com/madler/zlib.git
  ${WINDOWS_ITANIUM_ZLIB_VERSION}
  TARGET zlibstatic
  OPTIONS -DZLIB_BUILD_EXAMPLES=OFF)

windows_itanium_build_dependency(zstd https://github.com/facebook/zstd.git
  ${WINDOWS_ITANIUM_ZSTD_VERSION}
  SOURCE_SUBDIR build/cmake
  OPTIONS -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
          -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON)

windows_itanium_build_dependency(libxml2 https://github.com/GNOME/libxml2.git
  ${WINDOWS_ITANIUM_LIBXML2_VERSION}
  OPTIONS -DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_LZMA=OFF
          -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_PYTHON=OFF
          -DLIBXML2_WITH_MODULES=OFF -DLIBXML2_WITH_PROGRAMS=OFF
          -DLIBXML2_WITH_TESTS=OFF)

# Name each library, so that LLVM's find modules take the static ones.
file(GLOB zlib_library "${deps_install_dir}/lib/*zlibstatic*.lib")
file(GLOB zstd_library "${deps_install_dir}/lib/*zstd*.lib")
file(GLOB libxml2_library "${deps_install_dir}/lib/*xml2*.lib")
set(ZLIB_LIBRARY "${zlib_library}" CACHE FILEPATH "")
set(ZLIB_INCLUDE_DIR "${deps_install_dir}/include" CACHE PATH "")
set(zstd_LIBRARY "${zstd_library}" CACHE FILEPATH "")
set(zstd_STATIC_LIBRARY "${zstd_library}" CACHE FILEPATH "")
set(zstd_INCLUDE_DIR "${deps_install_dir}/include" CACHE PATH "")
set(LIBXML2_LIBRARY "${libxml2_library}" CACHE FILEPATH "")
set(LIBXML2_STATIC_LIBRARY "${libxml2_library}" CACHE FILEPATH "")
set(LIBXML2_INCLUDE_DIR "${deps_install_dir}/include/libxml2" CACHE PATH "")
set(LIBXML2_DEFINITIONS "-DLIBXML_STATIC" CACHE STRING "")
set(PC_LIBXML_STATIC_LIBRARIES bcrypt CACHE STRING "")
# These are the whole description of each library; a pkg-config on the host
# describes the host's own.
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON CACHE BOOL "")
set(LLVM_USE_STATIC_ZSTD ON CACHE BOOL "")

# The first stage of a bootstrap has the second stage load this file too, with
# the first stage's clang and the second stage's triple.
if(CLANG_ENABLE_BOOTSTRAP AND NOT WINDOWS_ITANIUM_DEPENDENCIES_TOOLS)
  set(BOOTSTRAP_WINDOWS_ITANIUM_DEPENDENCIES_TOOLS "${CMAKE_BINARY_DIR}/bin"
      CACHE PATH "")
  set(BOOTSTRAP_WINDOWS_ITANIUM_DEPENDENCIES_TARGET
      "${BOOTSTRAP_LLVM_HOST_TRIPLE}" CACHE STRING "")
  set(BOOTSTRAP_WINDOWS_ITANIUM_DEPENDENCIES_SOURCE_DIR
      "${WINDOWS_ITANIUM_DEPENDENCIES_SOURCE_DIR}" CACHE PATH "")
  if(NOT CLANG_BOOTSTRAP_CMAKE_ARGS MATCHES "WindowsItanium-dependencies")
    set(CLANG_BOOTSTRAP_CMAKE_ARGS
        -C ${CMAKE_CURRENT_LIST_FILE} ${CLANG_BOOTSTRAP_CMAKE_ARGS}
        CACHE STRING "" FORCE)
  endif()
endif()
