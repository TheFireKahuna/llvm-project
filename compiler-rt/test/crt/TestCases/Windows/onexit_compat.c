// Test _onexit compatibility function.
//
// RUN: %clang_crt_main %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

// _onexit function type and declaration.
typedef int (*_onexit_t)(void);
extern _onexit_t _onexit(_onexit_t func);

static int g_order = 0;

int handler_first(void) {
  printf("handler_first at order %d\n", g_order++);
  return 0;
}

int handler_second(void) {
  printf("handler_second at order %d\n", g_order++);
  return 0;
}

int handler_third(void) {
  printf("handler_third at order %d\n", g_order++);
  return 0;
}

int main(void) {
  // CHECK: _onexit compatibility test
  printf("_onexit compatibility test\n");

  // _onexit handlers run in LIFO order (same as atexit).
  _onexit_t ret1 = _onexit(handler_first);
  _onexit_t ret2 = _onexit(handler_second);
  _onexit_t ret3 = _onexit(handler_third);

  // CHECK: registrations succeeded = 1
  printf("registrations succeeded = %d\n",
         ret1 != NULL && ret2 != NULL && ret3 != NULL);

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// Handlers run in reverse order (LIFO).
// CHECK: handler_third at order 0
// CHECK: handler_second at order 1
// CHECK: handler_first at order 2
