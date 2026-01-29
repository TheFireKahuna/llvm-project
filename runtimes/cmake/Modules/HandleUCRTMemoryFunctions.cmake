# HandleUCRTMemoryFunctions.cmake
#
# On Windows, the mem* functions (memcpy, memset, memmove, memcmp) are provided
# by vcruntime.lib. However, vcruntime.lib also contains MSVC C++ exception
# handling and RTTI symbols that conflict with Itanium ABI runtimes.
#
# ucrtbase.dll exports these same mem* functions, but Microsoft's ucrt.lib
# import library doesn't expose them - they expect you to use vcruntime.lib.
#
# For non-MSVC Windows targets (like windows-itanium) that use the Itanium C++
# ABI, we generate a minimal import library that imports mem* from ucrtbase.dll
# instead of vcruntime.dll.

function(generate_ucrt_memory_import_library output_var)
  if(NOT WIN32 OR MINGW OR MSVC)
    set(${output_var} "" PARENT_SCOPE)
    return()
  endif()

  set(def_content "LIBRARY ucrtbase\nEXPORTS\n    memcpy\n    memmove\n    memset\n    memcmp\n    memchr\n")
  set(def_file "${CMAKE_CURRENT_BINARY_DIR}/ucrt_memory.def")
  set(output_lib "${CMAKE_CURRENT_BINARY_DIR}/ucrt_memory.lib")

  if(NOT EXISTS "${output_lib}")
    file(WRITE "${def_file}" "${def_content}")

    # Determine architecture
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(machine "X64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
      set(machine "X86")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
      set(machine "ARM64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM")
      set(machine "ARM")
    else()
      message(WARNING "Unknown architecture for ucrt_memory.lib generation")
      set(${output_var} "" PARENT_SCOPE)
      return()
    endif()

    # Try llvm-lib first (works in cross-compilation), then lib.exe
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
