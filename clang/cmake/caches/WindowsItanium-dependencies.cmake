# This file builds a static libxml2 with the host's compiler while the cache is
# loaded, and points the LLVM build at it. It belongs to this fork and is not
# meant for upstream, whose caches take a dependency from the host. Load it
# after the cache that needs libxml2:
#
#   cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -C clang/cmake/caches/WindowsItanium-runtimes.cmake \
#     -C clang/cmake/caches/WindowsItanium-dependencies.cmake llvm
#
# The sources are cloned into WINDOWS_ITANIUM_DEPENDENCIES_SOURCE_DIR, which
# may be filled beforehand for a build without network access.

set(version 2.14.6)
set(WINDOWS_ITANIUM_DEPENDENCIES_SOURCE_DIR
    "${CMAKE_BINARY_DIR}/windows-itanium-deps/src" CACHE PATH
    "Sources of the Windows Itanium caches' dependencies")
set(source_dir "${WINDOWS_ITANIUM_DEPENDENCIES_SOURCE_DIR}/libxml2-${version}")
set(build_dir "${CMAKE_BINARY_DIR}/windows-itanium-deps/build/libxml2")
set(install_dir "${CMAKE_BINARY_DIR}/windows-itanium-deps/host")

if(NOT EXISTS "${install_dir}/include/libxml2")
  if(NOT EXISTS "${source_dir}")
    execute_process(
      COMMAND git -c advice.detachedHead=false clone --quiet --depth 1
              --branch v${version} https://github.com/GNOME/libxml2.git
              ${source_dir}
      COMMAND_ERROR_IS_FATAL ANY)
  endif()
  # A static library cannot carry the version resource to the images that
  # link it.
  file(READ "${source_dir}/CMakeLists.txt" contents)
  string(REPLACE "if(WIN32)\n    list(APPEND LIBXML2_SRCS win32/libxml2.rc)"
         "if(WIN32 AND BUILD_SHARED_LIBS)\n    list(APPEND LIBXML2_SRCS win32/libxml2.rc)"
         contents "${contents}")
  file(WRITE "${source_dir}/CMakeLists.txt" "${contents}")

  set(compiler)
  if(CMAKE_C_COMPILER)
    set(compiler -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
  endif()
  message(STATUS "Building libxml2 ${version}")
  file(MAKE_DIRECTORY "${build_dir}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -G Ninja -S ${source_dir} -B ${build_dir}
            ${compiler} -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
            -DCMAKE_INSTALL_PREFIX=${install_dir}
            -DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_LZMA=OFF
            -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_PYTHON=OFF
            -DLIBXML2_WITH_MODULES=OFF -DLIBXML2_WITH_PROGRAMS=OFF
            -DLIBXML2_WITH_TESTS=OFF
    OUTPUT_FILE ${build_dir}/configure.log ERROR_FILE ${build_dir}/configure.log
    COMMAND_ERROR_IS_FATAL ANY)
  execute_process(
    COMMAND ${CMAKE_COMMAND} --build ${build_dir} --target install
    OUTPUT_FILE ${build_dir}/build.log ERROR_FILE ${build_dir}/build.log
    COMMAND_ERROR_IS_FATAL ANY)
endif()

# Name the library, so that LLVM's find module takes this one, and what a
# static libxml2 needs of its users, which the module has from pkg-config.
set(LIBXML2_LIBRARY "${install_dir}/lib/libxml2s.lib" CACHE FILEPATH "")
set(LIBXML2_STATIC_LIBRARY "${LIBXML2_LIBRARY}" CACHE FILEPATH "")
set(LIBXML2_INCLUDE_DIR "${install_dir}/include/libxml2" CACHE PATH "")
set(LIBXML2_DEFINITIONS -DLIBXML_STATIC CACHE STRING "")
set(PC_LIBXML_STATIC_LIBRARIES bcrypt CACHE STRING "")
