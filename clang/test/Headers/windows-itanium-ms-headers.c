// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fms-extensions \
// RUN:     -fms-compatibility -fms-compatibility-version=19.33 \
// RUN:     -internal-isystem %S/Inputs/include -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-msvc -fms-extensions \
// RUN:     -fms-compatibility -fms-compatibility-version=19.33 \
// RUN:     -internal-isystem %S/Inputs/include -fsyntax-only -verify \
// RUN:     -x c++ %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu \
// RUN:     -internal-isystem %S/Inputs/include -fsyntax-only -verify %s
// expected-no-diagnostics

// Windows Itanium has no Visual C++ headers, so clang's intrin.h, intrin0.h,
// arm64intr.h and vadefs.h provide their contents themselves, as they do for
// MSVC targets, while float.h includes the C library's, as it does for MSVC
// and MinGW but not for Linux.

#include <stddef.h>
#include <float.h>

#ifdef __cplusplus
#define _Static_assert static_assert
#endif

#if defined(_MSC_VER) || defined(_WIN32_ITANIUM)
_Static_assert(FLOAT_LOCAL_DEF, "");
#elif defined(FLOAT_LOCAL_DEF)
#error "only Windows targets include the C library's float.h"
#endif

#if defined(_MSC_VER) || defined(_WIN32_ITANIUM)
#include <intrin.h>
#ifdef __x86_64__
_Static_assert(_XCR_XFEATURE_ENABLED_MASK == 0, "");
#endif
void intrinsics(void) {
  __nop();
  __debugbreak();
}
#endif

#ifdef _WIN32_ITANIUM
#include <vadefs.h>
_Static_assert(sizeof(uintptr_t) == sizeof(void *), "");
_Static_assert(_CRT_PACKING == 8, "");
int first(int n, ...) {
  va_list ap;
  __crt_va_start(ap, n);
  int r = __crt_va_arg(ap, int);
  __crt_va_end(ap);
  return r;
}
#endif
