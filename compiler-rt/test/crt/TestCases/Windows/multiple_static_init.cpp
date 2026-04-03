// Test multiple static objects with complex initialization dependencies.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

static int g_init_order = 0;
static int g_dtor_order = 0;

struct TrackedObject {
  int id;
  int init_seq;
  TrackedObject(int i) : id(i), init_seq(g_init_order++) {
    printf("TrackedObject(%d) init_seq=%d\n", id, init_seq);
  }
  ~TrackedObject() {
    printf("TrackedObject(%d) dtor_seq=%d\n", id, g_dtor_order++);
  }
};

// Define objects in non-sequential order to verify alphabetical sorting
// doesn't affect user-defined initialization order.
static TrackedObject obj_c(3);
static TrackedObject obj_a(1);
static TrackedObject obj_b(2);

// Function-local static with lazy initialization.
TrackedObject &get_lazy() {
  static TrackedObject lazy_obj(100);
  return lazy_obj;
}

// Nested static in lambda.
auto make_lambda_static = []() -> TrackedObject & {
  static TrackedObject lambda_static(200);
  return lambda_static;
};

int main() {
  // CHECK: Multiple static initialization test
  printf("Multiple static initialization test\n");

  // Global statics should already be constructed in definition order.
  // CHECK: TrackedObject(3) init_seq=0
  // CHECK: TrackedObject(1) init_seq=1
  // CHECK: TrackedObject(2) init_seq=2

  // Access function-local static - triggers lazy init.
  // CHECK: TrackedObject(100) init_seq=3
  printf("lazy.id = %d\n", get_lazy().id);

  // Access lambda static - triggers lazy init.
  // CHECK: TrackedObject(200) init_seq=4
  printf("lambda_static.id = %d\n", make_lambda_static().id);

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// Destruction order is reverse of construction (LIFO):
// CHECK: TrackedObject(200) dtor_seq=0
// CHECK: TrackedObject(100) dtor_seq=1
// CHECK: TrackedObject(2) dtor_seq=2
// CHECK: TrackedObject(1) dtor_seq=3
// CHECK: TrackedObject(3) dtor_seq=4
