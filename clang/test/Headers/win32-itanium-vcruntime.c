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
// RUN:     -fsyntax-only -verify -x c++ -std=c++03 %s

// Windows Itanium's vcruntime.h gives the UCRT headers the types, macros and
// declaration brackets they expect from the Visual C++ runtime.

#include <corecrt.h>

#ifdef __cplusplus
#if __cplusplus >= 201103L
#define _Static_assert static_assert
#endif
// The UCRT's declarations have C linkage.
extern "C" size_t __ucrt_function(wchar_t const *, va_list);
#endif

_Static_assert(sizeof(size_t) == sizeof(void *), "");
_Static_assert(sizeof(ptrdiff_t) == sizeof(void *), "");
_Static_assert(sizeof(intptr_t) == sizeof(void *), "");
_Static_assert(sizeof(uintptr_t) == sizeof(void *), "");
_Static_assert(sizeof(wchar_t) == 2, "");
// Declarations between _CRT_BEGIN_C_HEADER and _CRT_END_C_HEADER are packed
// to 8 bytes, and later ones are not.
_Static_assert(sizeof(struct __ucrt_packed) == 24, "");
struct unpacked {
  char c;
  __int128 x;
};
_Static_assert(sizeof(struct unpacked) == 32, "");

_Static_assert(sizeof(_CRT_WIDE("a")) == 2 * sizeof(wchar_t), "");
_Static_assert(sizeof(_CRT_STRINGIZE(_CRT_PACKING)) == 2, "");
_Static_assert(_CRT_PACKING == 8, "");

// vadefs.h gives the variable argument macros the UCRT uses.
int first(int n, ...) {
  va_list ap;
  __crt_va_start(ap, n);
  int r = __crt_va_arg(ap, int);
  __crt_va_end(ap);
  return r;
}

// The UCRT's stdlib.h defines _countof in terms of __crt_countof. In C++ it
// rejects a pointer.
#define _countof __crt_countof
int array[5];
_Static_assert(_countof(array) == 5, "");
#ifdef __cplusplus
int *pointer;
// expected-error@+2 {{no matching function for call to '__countof_helper'}}
// expected-note@vcruntime.h:* {{candidate template ignored}}
size_t pointer_count = _countof(pointer);
#endif

// C++ gets the UCRT's inline functions without static, so that modules can
// export them; C keeps the UCRT's default.
#if defined(__cplusplus) && _STATIC_INLINE_UCRT_FUNCTIONS != 0
#error "C++ must see _STATIC_INLINE_UCRT_FUNCTIONS as 0"
#elif !defined(__cplusplus) && defined(_STATIC_INLINE_UCRT_FUNCTIONS)
#error "C must keep the UCRT's default"
#endif

#if defined(__cplusplus) && __cplusplus < 201103L
// The UCRT's noexcept declarations stay valid in C++03.
void no_throw() _CRT_NOEXCEPT;
#endif

size_t call(const wchar_t *s, va_list ap) {
  // The ISO C functions the UCRT marks as insecure are not deprecated.
  return __ucrt_function(s, ap);
}

void deprecated(void) {
  __ucrt_deprecated(); // expected-warning {{is deprecated: deprecated}}
  // expected-note@Inputs/win32_itanium/ucrt/corecrt.h:* {{deprecated here}}
}
