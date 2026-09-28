// RUN: %clang_cc1 -triple x86_64-pc-windows-msvc -fms-extensions \
// RUN:     -fms-compatibility-version=19.33 -ffreestanding -fsyntax-only \
// RUN:     -verify %s
// RUN: %clang_cc1 -triple x86_64-unknown-linux-gnu -ffreestanding \
// RUN:     -fsyntax-only -verify %s
// expected-no-diagnostics

// With Microsoft extensions _rotl and _mm_prefetch are builtins, which the
// Windows SDK and the UCRT declare as functions, so the headers must not
// define them as macros. Without the extensions, the headers provide them.

#include <x86intrin.h>

#ifdef _MSC_EXTENSIONS
unsigned int __cdecl _rotl(unsigned int, int);
unsigned long __cdecl _lrotr(unsigned long, int);
void _mm_prefetch(char const *, int);
#endif

unsigned int rotate(unsigned int x) {
  return _rotl(x, 3) + _rotr(x, 3) + (unsigned int)_lrotr(x, 3);
}

void prefetch(const char *p) { _mm_prefetch(p, _MM_HINT_T0); }
