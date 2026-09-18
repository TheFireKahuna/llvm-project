// Test quick_exit and at_quick_exit.
//
// RUN: %clangxx_crt_main -x c++ %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// RUN: %clang_crt_main %s -o %t.c.exe
// RUN: %run %t.c.exe | FileCheck %s
// REQUIRES: windows, crt

#include <stdio.h>
#include <stdlib.h>

static int g_order = 0;

void quick_handler_1(void) {
  printf("quick_handler_1 at %d\n", g_order++);
}

void quick_handler_2(void) {
  printf("quick_handler_2 at %d\n", g_order++);
}

#ifdef __cplusplus
struct StaticDestructor {
  ~StaticDestructor() {
    // This should NOT run with quick_exit.
    printf("StaticDestructor - SHOULD NOT PRINT\n");
  }
};

static StaticDestructor g_dtor;
#endif

int main(void) {
  // quick_exit does not flush buffered stdout.
  setvbuf(stdout, NULL, _IONBF, 0);
  // CHECK: quick_exit test
  printf("quick_exit test\n");

  // Register handlers - LIFO order.
  if (at_quick_exit(quick_handler_1) || at_quick_exit(quick_handler_2))
    return 2;

  // CHECK: calling quick_exit
  printf("calling quick_exit\n");

  // quick_exit runs at_quick_exit handlers but NOT atexit/destructors.
  quick_exit(0);

  // Should not reach here.
  printf("Should not print\n");
  return 1;
}

// CHECK: quick_handler_2 at 0
// CHECK: quick_handler_1 at 1
// CHECK-NOT: StaticDestructor
// CHECK-NOT: Should not print
