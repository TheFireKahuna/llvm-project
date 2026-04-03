// Test CFG-protected indirect calls.
//
// RUN: %clang_crt_main -Xclang -cfguard %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

typedef int (*func_ptr_t)(int);

__attribute__((noinline))
int add_one(int x) {
  return x + 1;
}

__attribute__((noinline))
int multiply_two(int x) {
  return x * 2;
}

__attribute__((noinline))
int subtract_three(int x) {
  return x - 3;
}

__attribute__((noinline))
int call_indirect(func_ptr_t fn, int arg) {
  return fn(arg);
}

static func_ptr_t functions[] = {add_one, multiply_two, subtract_three};

int main() {
  printf("CFG indirect call test\n");

  int result;

  func_ptr_t fp = add_one;
  result = fp(10);
  // CHECK: direct call result = 11
  printf("direct call result = %d\n", result);

  result = call_indirect(multiply_two, 7);
  // CHECK: indirect call result = 14
  printf("indirect call result = %d\n", result);

  result = functions[2](20);
  // CHECK: array call result = 17
  printf("array call result = %d\n", result);

  int total = 0;
  for (int i = 0; i < 3; i++) {
    total += call_indirect(functions[i], 5);
  }
  // CHECK: loop total = 18
  printf("loop total = %d\n", total);

  result = call_indirect(subtract_three, call_indirect(add_one, 10));
  // CHECK: nested call result = 8
  printf("nested call result = %d\n", result);

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
