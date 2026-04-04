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

include(GenerateImportLibrary)

function(generate_ucrt_memory_import_library output_var)
  set(_def_content "LIBRARY ucrtbase.dll
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
  generate_import_library(${output_var} ucrt_memory "${_def_content}")
  set(${output_var} "${${output_var}}" PARENT_SCOPE)
endfunction()
