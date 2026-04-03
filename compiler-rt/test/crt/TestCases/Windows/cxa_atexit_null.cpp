// Test __cxa_atexit handling of null destructor.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

extern "C" int __cxa_atexit(void (*)(void *), void *, void *);
extern "C" void *__dso_handle;

static int g_call_count = 0;

void valid_dtor(void *arg) {
  int id = *static_cast<int *>(arg);
  g_call_count++;
  printf("valid_dtor(%d) called, count=%d\n", id, g_call_count);
}

int main() {
  // CHECK: __cxa_atexit null test
  printf("__cxa_atexit null test\n");

  static int id1 = 1, id2 = 2, id3 = 3;

  // Register valid handler.
  int ret1 = __cxa_atexit(valid_dtor, &id1, __dso_handle);
  // CHECK: reg1 = 0
  printf("reg1 = %d\n", ret1);

  // Register null handler - should fail or be ignored.
  int ret2 = __cxa_atexit(nullptr, &id2, __dso_handle);
  // CHECK: reg2 = -1
  printf("reg2 = %d\n", ret2);

  // Register another valid handler.
  int ret3 = __cxa_atexit(valid_dtor, &id3, __dso_handle);
  // CHECK: reg3 = 0
  printf("reg3 = %d\n", ret3);

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// Only valid handlers should run.
// CHECK: valid_dtor(3) called, count=1
// CHECK: valid_dtor(1) called, count=2
// CHECK-NOT: valid_dtor(2)
