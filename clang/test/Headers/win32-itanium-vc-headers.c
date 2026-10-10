// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -fsyntax-only -verify -x c++ %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:     -internal-isystem %resource_dir/win32_itanium_wrappers \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/ucrt \
// RUN:     -internal-isystem %S/Inputs/win32_itanium/shared \
// RUN:     -fsyntax-only -verify -x c++ %s
// expected-no-diagnostics

// Windows Itanium's stand-ins for the Visual C++ runtime headers that the UCRT
// and the Windows SDK include.

#include <excpt.h>
#include <sal.h>
#include <setjmp.h>
#include <specstrings_strict.h>
#include <stddef.h>
#include <vcruntime_startup.h>
#include <vcruntime_string.h>

#ifdef __cplusplus
#define _Static_assert static_assert
#endif

// The SDK's annotation headers define __null, which NULL uses in C++.
void *null_pointer = NULL;

_Static_assert(sizeof(_JUMP_BUFFER) <= sizeof(jmp_buf), "");
#ifdef __x86_64__
_Static_assert(sizeof(jmp_buf) == 256, "");
_Static_assert(_Alignof(jmp_buf) == 16, "");
#else
_Static_assert(sizeof(jmp_buf) == 192, "");
#endif

_crt_argv_mode argv_mode = _crt_argv_unexpanded_arguments;

EXCEPTION_DISPOSITION disposition(void) {
  return ExceptionContinueSearch;
}

unsigned long filter(void) {
  __try {
  } __except (GetExceptionCode() == 0 ? EXCEPTION_EXECUTE_HANDLER
                                      : EXCEPTION_CONTINUE_SEARCH) {
  }
  return 0;
}

#ifdef __cplusplus
// C++ gets the const-correct forms and C linkage.
extern "C" const char *strchr(const char *, int);
#endif

size_t scan(const char *s) {
  return (size_t)(strchr(s, 'a') - s) + (size_t)memcmp(s, s, 1);
}
