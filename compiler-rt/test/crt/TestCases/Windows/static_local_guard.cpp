// Test static local variable guard variables work correctly.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

static int g_construct_order = 0;

struct Counter {
  int id;
  int construct_seq;

  Counter(int i) : id(i), construct_seq(g_construct_order++) {
    printf("Counter(%d) constructed at seq %d\n", id, construct_seq);
  }

  ~Counter() {
    printf("Counter(%d) destructed\n", id);
  }

  int get() const { return id; }
};

// Function with static local.
int get_value_1() {
  static Counter c(100);
  return c.get();
}

// Another function with static local.
int get_value_2() {
  static Counter c(200);
  return c.get();
}

// Recursive function with static local.
int recursive_static(int depth) {
  static Counter c(300);  // Only constructed once.

  if (depth > 0) {
    return c.get() + recursive_static(depth - 1);
  }
  return c.get();
}

// Function that may or may not initialize its static.
int conditional_static(bool init) {
  if (init) {
    static Counter c(400);
    return c.get();
  }
  return -1;
}

int main() {
  // CHECK: Static local guard test
  printf("Static local guard test\n");

  // First call should construct.
  // CHECK: Counter(100) constructed at seq 0
  // CHECK: get_value_1 = 100
  printf("get_value_1 = %d\n", get_value_1());

  // Second call should reuse.
  // CHECK-NOT: Counter(100) constructed
  // CHECK: get_value_1 again = 100
  printf("get_value_1 again = %d\n", get_value_1());

  // Different function, different static.
  // CHECK: Counter(200) constructed at seq 1
  // CHECK: get_value_2 = 200
  printf("get_value_2 = %d\n", get_value_2());

  // Recursive calls should only construct once.
  // CHECK: Counter(300) constructed at seq 2
  // CHECK: recursive result = 1500
  printf("recursive result = %d\n", recursive_static(4));  // 300 * 5

  // Conditional: don't init.
  // CHECK: conditional false = -1
  printf("conditional false = %d\n", conditional_static(false));

  // Conditional: now init.
  // CHECK: Counter(400) constructed at seq 3
  // CHECK: conditional true = 400
  printf("conditional true = %d\n", conditional_static(true));

  // Conditional: already initialized.
  // CHECK-NOT: Counter(400) constructed
  // CHECK: conditional true again = 400
  printf("conditional true again = %d\n", conditional_static(true));

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// Destructors run in reverse construction order.
// CHECK: Counter(400) destructed
// CHECK: Counter(300) destructed
// CHECK: Counter(200) destructed
// CHECK: Counter(100) destructed
