# CopyRuntimeDLLs.cmake - Copy runtime DLLs for bootstrap builds
#
# Invoked as: cmake -DSRC_DIR=<staging> -DDST_DIR=<stage2-bin> -P CopyRuntimeDLLs.cmake
#
# Copies all .dll files from SRC_DIR to DST_DIR. Used during multi-stage
# bootstrap builds where stage2 executables dynamically link against runtime
# libraries (libc++, libunwind, etc.) built by the runtimes sub-build.
# PE/COFF has no RPATH equivalent, so DLLs must be placed next to executables.

if(NOT DEFINED SRC_DIR)
  message(FATAL_ERROR "SRC_DIR not defined")
endif()
if(NOT DEFINED DST_DIR)
  message(FATAL_ERROR "DST_DIR not defined")
endif()

if(NOT IS_DIRECTORY "${SRC_DIR}")
  message(WARNING "CopyRuntimeDLLs: staging directory does not exist: ${SRC_DIR}")
  return()
endif()

file(MAKE_DIRECTORY "${DST_DIR}")
file(GLOB _dlls "${SRC_DIR}/*.dll")

if(NOT _dlls)
  message(WARNING "CopyRuntimeDLLs: no DLLs found in ${SRC_DIR}")
  return()
endif()

foreach(_dll IN LISTS _dlls)
  get_filename_component(_name "${_dll}" NAME)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "${DST_DIR}/${_name}"
    RESULT_VARIABLE _result
  )
  if(_result EQUAL 0)
    message(STATUS "  ${_name}")
  else()
    message(WARNING "CopyRuntimeDLLs: failed to copy ${_name}")
  endif()
endforeach()

list(LENGTH _dlls _count)
message(STATUS "Copied ${_count} runtime DLL(s) to ${DST_DIR}")
