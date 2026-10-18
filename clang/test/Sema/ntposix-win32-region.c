// RUN: %clang_cc1 -triple x86_64-pc-windows-ntposix -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-pc-windows-ntposix -fsyntax-only -verify %s

// A file that '#pragma clang win32_header' marks is a Win32 declaration
// region: there long is 32 bits, long double a double and wchar_t 16 bits, and
// the Win32 predefines are in effect. Outside it the unit keeps LP64.
#include <stddef.h>
#include "Inputs/ntposix-win32/region.h"

#ifdef _WIN32
#error the Win32 predefines end with the region
#endif

_Static_assert(sizeof(long) == 8, "the unit's long is 64 bits");
_Static_assert(sizeof(wchar_t) == 4, "the unit's wchar_t is 32 bits");
_Static_assert(sizeof(LONG) == 4, "a typedef keeps the region's long");

// A wide literal a region's macro pastes is the region's.
_Static_assert(sizeof(REGION_TEXT("ab")) == 6, "pasted by the region");
_Static_assert(sizeof(L"ab") == 12, "written by the unit");

// In C a region type is compatible with its twin, as an enum is with its
// underlying type.
int *as_int(LONG *p) { return p; }
LONG take_long(int *value);
unsigned short *as_ushort(WCHAR *p) { return p; }
double *as_double(LDOUBLE *p) { return p; }

// A region type is not compatible with the unit's long.
long *as_long(LONG *p) {
  return p; // expected-error {{incompatible pointer types returning 'LONG *'}}
}
