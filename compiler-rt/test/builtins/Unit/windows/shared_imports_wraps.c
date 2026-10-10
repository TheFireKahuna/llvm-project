// RUN: %if ntdllp %{ %python %S/Inputs/shared_imports.py "%wincrt_libdir" \
// RUN:     "%ucrt_lib" "%ntdllp_lib" | FileCheck %s --check-prefix=NTDLLP %}
// RUN: %if vcruntime %{ %python %S/Inputs/shared_imports.py "%wincrt_libdir" \
// RUN:     "%ucrt_lib" "%vcruntime_lib" | FileCheck %s --check-prefix=VCRT %}
// REQUIRES: ucrt-lib

// Every Universal CRT function that an image imports and that the Windows
// SDK's private ntdllp.lib or Visual C++'s vcruntime.lib also imports, from
// ntdll.dll or vcruntime140.dll, is wrapped by each entry object, and
// clang_rt.ucrt_memory.lib imports each __wrap_ name as the function, from
// the DLL that ucrt.lib or clang_rt.ucrt_memory.lib names for it. The script
// fails on any name such a library offers that is not.

// NTDLLP: wrapped {{[0-9]+}}: __C_specific_handler {{.*}} _setjmp {{.*}} memset {{.*}} strlen
// VCRT: wrapped 15: __C_specific_handler _get_purecall_handler _set_purecall_handler longjmp memchr memcmp memcpy memmove memset strchr strrchr strstr wcschr wcsrchr wcsstr
