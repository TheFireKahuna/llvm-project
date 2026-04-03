# HandleUCRTMemoryFunctions.cmake
#
# Generate import library for vcruntime-equivalent symbols from ucrtbase.dll.
# Avoids vcruntime.lib which has MSVC EH/RTTI symbols conflicting with Itanium ABI.
#
# These symbols are exported from ucrtbase.dll but NOT in ucrt.lib (Microsoft
# expects them from vcruntime.lib). For Windows Itanium, we import them directly
# from ucrtbase.dll via this generated import library.
#
# NOT included:
#   _purecall - wincrt provides its own that bridges to __cxa_pure_virtual
#   _amsg_exit - not exported from any DLL, wincrt provides implementation

function(generate_ucrt_memory_import_library output_var)
  if(NOT WIN32 OR MINGW OR MSVC)
    set(${output_var} "" PARENT_SCOPE)
    return()
  endif()

  set(def_content "LIBRARY ucrtbase
EXPORTS
    ; Memory functions (compiler intrinsic fallbacks)
    memcpy
    memmove
    memset
    memcmp
    memchr
    ; SEH personality handlers
    __C_specific_handler
    __C_specific_handler_noexcept
    ; MSVC EH state (for foreign exception interop)
    __current_exception
    __current_exception_context
    ; RTTI support
    __std_type_info_destroy_list
")
  set(def_file "${CMAKE_CURRENT_BINARY_DIR}/ucrt_memory.def")
  set(output_lib "${CMAKE_CURRENT_BINARY_DIR}/ucrt_memory.lib")

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
      message(WARNING "Unknown architecture for ucrt_memory.lib generation: ARCH_ID='${_arch_id}'")
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
        message(WARNING "Failed to generate ucrt_memory.lib: ${error}")
        set(${output_var} "" PARENT_SCOPE)
        return()
      endif()
    else()
      message(WARNING "Could not find llvm-lib or lib.exe to generate ucrt_memory.lib")
      set(${output_var} "" PARENT_SCOPE)
      return()
    endif()
  endif()

  set(${output_var} "${output_lib}" PARENT_SCOPE)
endfunction()
