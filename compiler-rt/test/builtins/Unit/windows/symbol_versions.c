// RUN: rm -rf %t.r1 %t.r2 && mkdir %t.r1 %t.r2
// RUN: %clang_wincrt -shared -DRELEASE=1 %s -o %t.r1/lib.dll
// RUN: %clang_wincrt -shared -DRELEASE=2 %s -o %t.r2/lib.dll
// RUN: %clang_wincrt %s %t.r1/lib.dll.lib -o %t.r1/app1.exe
// RUN: %clang_wincrt %s %t.r2/lib.dll.lib -o %t.r2/app2.exe
// RUN: %run %t.r1/app1.exe | FileCheck --check-prefix=V1 %s
// RUN: cp %t.r1/app1.exe %t.r2/app1.exe
// RUN: %run %t.r2/app1.exe | FileCheck --check-prefix=V1 %s
// RUN: %run %t.r2/app2.exe | FileCheck --check-prefix=V2 %s
// RUN: cp %t.r2/app2.exe %t.r1/app2.exe
// RUN: not %run %t.r1/app2.exe

// A program binds the default version of a DLL's function that it was linked
// against: one linked against the first release keeps calling version 1 when
// the second release makes version 2 the default, and one linked against the
// second release fails to load against the first, which lacks version 2.

// V1: foo 1
// V2: foo 2

#include <stdio.h>

#ifdef RELEASE
#if RELEASE == 1
__attribute__((visibility("default"), symver("foo@@LIB_1"))) int
foo_v1(void) {
  return 1;
}
#else
__attribute__((visibility("default"), symver("foo@LIB_1"))) int foo_v1(void) {
  return 1;
}
__attribute__((visibility("default"), symver("foo@@LIB_2"))) int
foo_v2(void) {
  return 2;
}
#endif
#else
__attribute__((visibility("default"))) int foo(void);

int main(void) {
  printf("foo %d\n", foo());
  return 0;
}
#endif
