# CMake rules for a clang that targets *-windows-itanium or *-windows-ntposix,
# loaded through CMAKE_USER_MAKE_RULES_OVERRIDE. CMake has no rules for either
# environment and applies MinGW's, which hold but for the names of libraries
# and the options they give GNU ld: the driver links with lld-link.

set(MINGW FALSE)

# Every library keeps the lib prefix: libc++.dll, its import library
# libc++.dll.lib and the archive libc++.lib.
set(CMAKE_SHARED_LIBRARY_PREFIX "lib")
set(CMAKE_IMPORT_LIBRARY_PREFIX "lib")
set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
set(CMAKE_IMPORT_LIBRARY_SUFFIX ".dll.lib")
set(CMAKE_STATIC_LIBRARY_SUFFIX ".lib")

# lld-link's option for the import library, and none for the image version.
foreach(lang C CXX)
  foreach(rule CREATE_SHARED_LIBRARY CREATE_SHARED_MODULE LINK_EXECUTABLE)
    string(REPLACE "-Wl,--out-implib," "-Wl,-implib:"
           CMAKE_${lang}_${rule} "${CMAKE_${lang}_${rule}}")
    string(REPLACE "${CMAKE_GNULD_IMAGE_VERSION}" ""
           CMAKE_${lang}_${rule} "${CMAKE_${lang}_${rule}}")
  endforeach()
endforeach()
