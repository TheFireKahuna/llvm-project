// RUN: %clang_cc1 -triple x86_64-unknown-windows-itanium -fms-extensions \
// RUN:   -internal-isystem %S/Inputs/include -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-unknown-windows-itanium -fms-extensions \
// RUN:   -internal-isystem %S/Inputs/include -fsyntax-only -verify -x c++ %s
// expected-no-diagnostics

// Windows Itanium has no Visual C++ headers and does not define _MSC_VER, so
// clang's intrinsic headers take their MSVC paths for it, and float.h
// includes the C library's.

#include <stddef.h>
#include <float.h>
#include <intrin.h>

#ifdef __cplusplus
#define _Static_assert static_assert
#endif

_Static_assert(FLOAT_LOCAL_DEF, "");

// The SDK declares these names as functions, which clang provides as builtins
// with Microsoft extensions.
#ifdef __x86_64__
unsigned int __cdecl _rotl(unsigned int, int);
void __cdecl _mm_prefetch(char const *, int);
_Static_assert(_XCR_XFEATURE_ENABLED_MASK == 0, "");
#endif

void intrinsics(void) {
  __nop();
  __debugbreak();
}
