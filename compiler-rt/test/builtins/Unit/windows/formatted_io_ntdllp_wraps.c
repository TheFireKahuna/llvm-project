// REQUIRES: ntdllp
// RUN: %python %S/Inputs/ntdllp_wraps.py "%ntdllp_lib" "%wincrt_libdir" \
// RUN:     | FileCheck %s

// Every name that the Windows SDK's private ntdllp.lib imports from ntdll.dll
// and clang_rt.wincrt_dynamic.dll exports is wrapped by each entry object, and
// both forms of the runtime define the wrapper, so that a program linking
// ntdllp.lib calls the runtime's definition. The script fails on any name the
// SDK offers that is not.

// CHECK: wrapped: {{.*}} sprintf {{.*}} swprintf
