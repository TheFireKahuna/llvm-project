// Verify CRT section sentinels and DSO handle are correctly defined.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdint.h>
#include <stdio.h>

// CRT section sentinels from crt_begin_windows.c / crt_end_windows.c.
typedef int (*_PIFV)(void);
typedef void (*_PVFV)(void);

extern _PIFV __xi_a[];
extern _PIFV __xi_z[];
extern _PVFV __xc_a[];
extern _PVFV __xc_z[];
extern _PVFV __xp_a[];
extern _PVFV __xp_z[];
extern _PVFV __xt_a[];
extern _PVFV __xt_z[];

// DSO handle: self-referential pointer for Itanium ABI.
extern void *__dso_handle;

// Floating-point marker.
extern int _fltused;

int main(void) {
  // CHECK: CRT sentinels test
  printf("CRT sentinels test\n");

  // Sentinel arrays exist and end > start.
  // CHECK: xi sentinels valid = 1
  printf("xi sentinels valid = %d\n",
         __xi_a != 0 && __xi_z != 0 && __xi_z >= __xi_a);

  // CHECK: xc sentinels valid = 1
  printf("xc sentinels valid = %d\n",
         __xc_a != 0 && __xc_z != 0 && __xc_z >= __xc_a);

  // CHECK: xp sentinels valid = 1
  printf("xp sentinels valid = %d\n",
         __xp_a != 0 && __xp_z != 0 && __xp_z >= __xp_a);

  // CHECK: xt sentinels valid = 1
  printf("xt sentinels valid = %d\n",
         __xt_a != 0 && __xt_z != 0 && __xt_z >= __xt_a);

  // __dso_handle should be self-referential.
  // CHECK: dso_handle self-ref = 1
  printf("dso_handle self-ref = %d\n", __dso_handle == &__dso_handle);

  // _fltused should have MSVC magic value.
  // CHECK: fltused magic = 1
  printf("fltused magic = %d\n", _fltused == 0x9875);

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
