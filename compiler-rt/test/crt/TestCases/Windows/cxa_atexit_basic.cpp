// Test basic __cxa_atexit registration and LIFO execution.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

extern "C" int __cxa_atexit(void (*)(void *), void *, void *);
extern "C" void *__dso_handle;

static int g_order = 0;

void dtor_a(void *arg) {
  int expected = *static_cast<int *>(arg);
  printf("dtor_a: expected=%d actual=%d\n", expected, g_order);
  g_order++;
}

void dtor_b(void *arg) {
  int expected = *static_cast<int *>(arg);
  printf("dtor_b: expected=%d actual=%d\n", expected, g_order);
  g_order++;
}

void dtor_c(void *arg) {
  int expected = *static_cast<int *>(arg);
  printf("dtor_c: expected=%d actual=%d\n", expected, g_order);
  g_order++;
}

int main() {
  printf("__cxa_atexit basic test\n");

  // Registration order: a, b, c. Execution order should be c, b, a (LIFO).
  static int order_a = 2;  // Expected to run third (g_order == 2)
  static int order_b = 1;  // Expected to run second (g_order == 1)
  static int order_c = 0;  // Expected to run first (g_order == 0)

  int ret_a = __cxa_atexit(dtor_a, &order_a, __dso_handle);
  int ret_b = __cxa_atexit(dtor_b, &order_b, __dso_handle);
  int ret_c = __cxa_atexit(dtor_c, &order_c, __dso_handle);

  // CHECK: registrations succeeded = 1
  printf("registrations succeeded = %d\n",
         ret_a == 0 && ret_b == 0 && ret_c == 0);

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// CHECK: dtor_c: expected=0 actual=0
// CHECK: dtor_b: expected=1 actual=1
// CHECK: dtor_a: expected=2 actual=2
