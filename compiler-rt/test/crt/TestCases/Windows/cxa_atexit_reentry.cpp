// Test __cxa_atexit reentry: registering new handlers during finalization.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

extern "C" int __cxa_atexit(void (*)(void *), void *, void *);
extern "C" void *__dso_handle;

void final_dtor(void *arg) {
  int id = *static_cast<int *>(arg);
  printf("final_dtor(%d)\n", id);
}

void reentrant_dtor(void *arg) {
  int id = *static_cast<int *>(arg);
  printf("reentrant_dtor(%d) - registering new handler\n", id);

  // Register a new handler during finalization. Per Itanium ABI, this should
  // work and the new handler should run in LIFO order.
  static int new_id = 100 + id;
  __cxa_atexit(final_dtor, &new_id, __dso_handle);
}

int main() {
  // CHECK: __cxa_atexit reentry test
  printf("__cxa_atexit reentry test\n");

  static int id1 = 1;
  static int id2 = 2;
  static int id3 = 3;

  __cxa_atexit(reentrant_dtor, &id1, __dso_handle);
  __cxa_atexit(reentrant_dtor, &id2, __dso_handle);
  __cxa_atexit(final_dtor, &id3, __dso_handle);

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// Expected order (LIFO with reentry):
// 1. final_dtor(3) - registered last, runs first
// 2. reentrant_dtor(2) - registers final_dtor(102)
// 3. final_dtor(102) - newly registered, runs next
// 4. reentrant_dtor(1) - registers final_dtor(101)
// 5. final_dtor(101) - newly registered, runs last

// CHECK: final_dtor(3)
// CHECK: reentrant_dtor(2) - registering new handler
// CHECK: final_dtor(102)
// CHECK: reentrant_dtor(1) - registering new handler
// CHECK: final_dtor(101)
