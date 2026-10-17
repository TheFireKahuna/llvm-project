// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
// RUN: %clang_wincrt -DOWN %s -o %t.own.exe
// RUN: %run %t.own.exe | FileCheck %s --check-prefix=OWN

// The Universal CRT declares onexit, _HUGE and HUGE, but ucrtbase.dll exports
// none of them; wincrt defines them. A program's own onexit and HUGE take
// precedence, even in an image that links the rest of wincrt's registration
// functions.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef OWN
onexit_t onexit(onexit_t Function) {
  printf("own onexit\n");
  return Function;
}
double HUGE = 1.0;
#endif

static int fromOnexit(void) {
  printf("onexit\n");
  return 0;
}
static void fromAtexit(void) { printf("atexit\n"); }

int main(void) {
  atexit(fromAtexit);
  if (onexit(fromOnexit) != fromOnexit)
    return 1;
  printf("HUGE %d, _HUGE %d\n", HUGE == HUGE_VAL, _HUGE == HUGE_VAL);
  return 0;
}

// CHECK:      HUGE 1, _HUGE 1
// CHECK-NEXT: onexit
// CHECK-NEXT: atexit

// OWN:      own onexit
// OWN-NEXT: HUGE 0, _HUGE 1
// OWN-NEXT: atexit
// OWN-NOT:  {{^}}onexit
