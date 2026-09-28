# Platform rules for a clang driver that targets *-windows-itanium or
# *-windows-ntposix, loaded through CMAKE_USER_MAKE_RULES_OVERRIDE by
# WindowsItaniumToolchain.cmake.
#
# No CMake release has platform rules for these environments, so by the time
# this file runs, for each enabled language, CMake has applied its MinGW rules
# (Platform/Windows-GNU.cmake). Compilation and archiving are right as they
# are. Linking is not: the driver links through lld-link, so the GNU ld
# options and the MinGW library names are replaced here with those of CMake's
# rules for a GNU-driver clang targeting MSVC (__windows_compiler_clang_gnu in
# Platform/Windows-Clang.cmake). Unlike those rules, these keep the driver's
# default libraries (no -nostdlib or -nostartfiles), which carry the C and C++
# runtimes and their startup code, and they add no MSVC runtime library
# options.

set(MINGW FALSE)

# Keep the lib prefix on every library, with the .dll.lib suffix telling an
# import library from the static library of the same name.
set(CMAKE_IMPORT_LIBRARY_PREFIX "lib")
set(CMAKE_SHARED_LIBRARY_PREFIX "lib")
set(CMAKE_SHARED_MODULE_PREFIX "lib")
set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
set(CMAKE_IMPORT_LIBRARY_SUFFIX ".dll.lib")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".lib")
set(CMAKE_FIND_LIBRARY_PREFIXES "lib" "")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll.lib" ".lib")
set(CMAKE_LINK_DEF_FILE_FLAG "-Xlinker /DEF:")

# lld-link resolves archive members in any order.
unset(CMAKE_LINK_GROUP_USING_RESCAN)
unset(CMAKE_LINK_GROUP_USING_RESCAN_SUPPORTED)

string(CONCAT _link_options
  "-Xlinker /MANIFEST:EMBED -Xlinker /implib:<TARGET_IMPLIB> "
  "-Xlinker /pdb:<TARGET_PDB> "
  "-Xlinker /version:<TARGET_VERSION_MAJOR>.<TARGET_VERSION_MINOR>")

foreach(lang C CXX)
  foreach(type SHARED_LIBRARY SHARED_MODULE EXE)
    unset(CMAKE_${type}_LINK_STATIC_${lang}_FLAGS)
    unset(CMAKE_${type}_LINK_DYNAMIC_${lang}_FLAGS)
  endforeach()

  set(CMAKE_${lang}_LINK_DEF_FILE_FLAG "${CMAKE_LINK_DEF_FILE_FLAG}")
  set(CMAKE_${lang}_LINKER_MANIFEST_FLAG " -Xlinker /MANIFESTINPUT:")
  set(CMAKE_${lang}_LINKER_SUPPORTS_PDB ON)
  set(CMAKE_${lang}_VERBOSE_LINK_FLAG "-v")

  # link.exe cannot link these images, so every linker choice is lld-link.
  foreach(linker DEFAULT SYSTEM LLD MSVC)
    set(CMAKE_${lang}_USING_LINKER_${linker} "-fuse-ld=lld-link")
  endforeach()

  string(CONCAT CMAKE_${lang}_CREATE_SHARED_LIBRARY
    "<CMAKE_${lang}_COMPILER> <CMAKE_SHARED_LIBRARY_${lang}_FLAGS> "
    "<LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> -o <TARGET> ${_link_options} "
    "<OBJECTS> <LINK_LIBRARIES> <MANIFESTS>")
  set(CMAKE_${lang}_CREATE_SHARED_MODULE
    "${CMAKE_${lang}_CREATE_SHARED_LIBRARY}")
  string(CONCAT CMAKE_${lang}_LINK_EXECUTABLE
    "<CMAKE_${lang}_COMPILER> <FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> "
    "${_link_options} <LINK_LIBRARIES> <MANIFESTS>")
  set(CMAKE_${lang}_CREATE_WIN32_EXE "-Xlinker /subsystem:windows")
  set(CMAKE_${lang}_CREATE_CONSOLE_EXE "-Xlinker /subsystem:console")
endforeach()
unset(_link_options)
