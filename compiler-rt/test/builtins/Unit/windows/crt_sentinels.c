// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe

// The builtins define the bounds of the .CRT$X?? tables, each a null entry,
// the image's __dso_handle, and the _fltused that objects compiled by Visual
// C++ name.

#include <stdint.h>

typedef int (*InitializerFn)(void);
typedef void (*TerminatorFn)(void);

extern const InitializerFn __xi_a[], __xi_z[];
extern const TerminatorFn __xc_a[], __xc_z[], __xp_a[], __xp_z[], __xt_a[],
    __xt_z[];
extern void *__dso_handle;
extern int _fltused;

int main(void) {
  // The linker orders the sections by name.
  const void *Bounds[] = {__xc_a, __xc_z, __xi_a, __xi_z,
                          __xp_a, __xp_z, __xt_a, __xt_z};
  for (int I = 1; I < 8; ++I)
    if ((uintptr_t)Bounds[I - 1] >= (uintptr_t)Bounds[I])
      return I;
  if (__xi_a[0] || __xi_z[0] || __xc_a[0] || __xc_z[0] || __xp_a[0] ||
      __xp_z[0] || __xt_a[0] || __xt_z[0])
    return 10;
  if (__dso_handle != &__dso_handle)
    return 11;
  if (_fltused != 0x9875)
    return 12;
  return 0;
}
