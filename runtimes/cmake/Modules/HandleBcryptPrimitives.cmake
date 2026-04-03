# HandleBcryptPrimitives.cmake
#
# Generate import library for bcryptprimitives.dll.
# The Windows SDK ships bcrypt.lib but not bcryptprimitives.lib.
# ProcessPrng (used for security cookie init) is exported from
# bcryptprimitives.dll, so we generate a minimal import library.

function(generate_bcryptprimitives_import_library output_var)
  if(NOT WIN32 OR MINGW OR MSVC)
    set(${output_var} "" PARENT_SCOPE)
    return()
  endif()

  set(def_content "LIBRARY bcryptprimitives.dll
EXPORTS
    ProcessPrng
")
  set(def_file "${CMAKE_CURRENT_BINARY_DIR}/bcryptprimitives.def")
  set(output_lib "${CMAKE_CURRENT_BINARY_DIR}/bcryptprimitives.lib")

  if(NOT EXISTS "${output_lib}")
    file(WRITE "${def_file}" "${def_content}")

    set(_arch_id "${CMAKE_C_COMPILER_ARCHITECTURE_ID}${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
    if(_arch_id MATCHES "x64|X64")
      set(machine "X64")
    elseif(_arch_id MATCHES "X86")
      set(machine "X86")
    elseif(_arch_id MATCHES "ARM64")
      set(machine "ARM64")
    elseif(_arch_id MATCHES "ARM")
      set(machine "ARM")
    else()
      message(WARNING "Unknown architecture for bcryptprimitives.lib: ARCH_ID='${_arch_id}'")
      set(${output_var} "" PARENT_SCOPE)
      return()
    endif()

    find_program(LIBEXE_TOOL llvm-lib HINTS ${LLVM_TOOLS_BINARY_DIR})
    if(NOT LIBEXE_TOOL)
      find_program(LIBEXE_TOOL lib)
    endif()

    if(LIBEXE_TOOL)
      execute_process(
        COMMAND ${LIBEXE_TOOL} /def:${def_file} /out:${output_lib} /machine:${machine}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
      )
      if(NOT result EQUAL 0)
        message(WARNING "Failed to generate bcryptprimitives.lib: ${error}")
        set(${output_var} "" PARENT_SCOPE)
        return()
      endif()
    else()
      message(WARNING "Could not find llvm-lib or lib.exe to generate bcryptprimitives.lib")
      set(${output_var} "" PARENT_SCOPE)
      return()
    endif()
  endif()

  set(${output_var} "${output_lib}" PARENT_SCOPE)
endfunction()
