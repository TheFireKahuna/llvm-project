# GenerateImportLibrary.cmake
#
# Shared helper to generate a minimal import library from a .def definition.
# Used by HandleBcryptPrimitives.cmake and HandleUCRTMemoryFunctions.cmake.

include_guard(GLOBAL)

# generate_import_library(<output_var> <lib_name> <def_content>)
#
# Generates a minimal import library at configure time using llvm-lib or
# lib.exe.  Sets <output_var> to the path of the generated .lib in the
# caller's scope, or "" on failure.
#
# This is a no-op on non-Windows, MinGW, and MSVC targets (which have their
# own SDK import libraries).
function(generate_import_library output_var lib_name def_content)
  if(NOT WIN32)
    set(${output_var} "" PARENT_SCOPE)
    return()
  endif()
  if(MINGW OR MSVC)
    set(${output_var} "" PARENT_SCOPE)
    return()
  endif()

  set(def_file "${CMAKE_CURRENT_BINARY_DIR}/${lib_name}.def")
  set(output_lib "${CMAKE_CURRENT_BINARY_DIR}/${lib_name}.lib")

  if(NOT EXISTS "${output_lib}")
    file(WRITE "${def_file}" "${def_content}")

    set(_arch_id "${CMAKE_C_COMPILER_ARCHITECTURE_ID}${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
    if(_arch_id MATCHES "[Xx]64")
      set(machine "X64")
    elseif(_arch_id MATCHES "X86")
      set(machine "X86")
    elseif(_arch_id MATCHES "ARM64")
      set(machine "ARM64")
    elseif(_arch_id MATCHES "ARM")
      set(machine "ARM")
    else()
      message(WARNING "generate_import_library: unknown architecture '${_arch_id}' for ${lib_name}.lib")
      set(${output_var} "" PARENT_SCOPE)
      return()
    endif()

    find_program(_GIL_LIB_TOOL NAMES llvm-lib lib
      HINTS ${LLVM_TOOLS_BINARY_DIR})

    if(_GIL_LIB_TOOL)
      execute_process(
        COMMAND ${_GIL_LIB_TOOL} /def:${def_file} /out:${output_lib} /machine:${machine}
        RESULT_VARIABLE _gil_result
        OUTPUT_VARIABLE _gil_output
        ERROR_VARIABLE _gil_error
      )
      if(NOT _gil_result EQUAL 0)
        message(WARNING "generate_import_library: failed to create ${lib_name}.lib: ${_gil_error}")
        set(${output_var} "" PARENT_SCOPE)
        return()
      endif()
    else()
      message(WARNING "generate_import_library: llvm-lib/lib.exe not found; cannot create ${lib_name}.lib")
      set(${output_var} "" PARENT_SCOPE)
      return()
    endif()
  endif()

  unset(_GIL_LIB_TOOL CACHE)
  set(${output_var} "${output_lib}" PARENT_SCOPE)
endfunction()
