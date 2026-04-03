// Test stack unwinding during exception propagation.
//
// RUN: %clang_crt_main -std=c++17 -fexceptions %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

static int g_dtor_order = 0;

struct StackObject {
  int id;
  int level;
  StackObject(int i, int l) : id(i), level(l) {
    printf("[%d] StackObject(%d) constructed at level %d\n",
           g_dtor_order, id, level);
  }
  ~StackObject() {
    printf("[%d] StackObject(%d) destructed from level %d\n",
           g_dtor_order++, id, level);
  }
};

void level3() {
  StackObject s3(3, 3);
  printf("level3: about to throw\n");
  throw 42;
}

void level2() {
  StackObject s2a(21, 2);
  StackObject s2b(22, 2);
  printf("level2: calling level3\n");
  level3();
  printf("level2: THIS SHOULD NOT PRINT\n");
}

void level1() {
  StackObject s1(1, 1);
  printf("level1: calling level2\n");
  level2();
  printf("level1: THIS SHOULD NOT PRINT\n");
}

int main() {
  // CHECK: Stack unwinding test
  printf("Stack unwinding test\n");

  try {
    StackObject main_obj(0, 0);
    printf("main: calling level1\n");
    level1();
    printf("main: THIS SHOULD NOT PRINT\n");
  } catch (int e) {
    printf("main: caught %d\n", e);
  }

  // Verify destruction order was LIFO during unwinding.
  // CHECK: [{{[0-9]+}}] StackObject(0) constructed at level 0
  // CHECK: main: calling level1
  // CHECK: [{{[0-9]+}}] StackObject(1) constructed at level 1
  // CHECK: level1: calling level2
  // CHECK: [{{[0-9]+}}] StackObject(21) constructed at level 2
  // CHECK: [{{[0-9]+}}] StackObject(22) constructed at level 2
  // CHECK: level2: calling level3
  // CHECK: [{{[0-9]+}}] StackObject(3) constructed at level 3
  // CHECK: level3: about to throw
  // CHECK: [0] StackObject(3) destructed from level 3
  // CHECK: [1] StackObject(22) destructed from level 2
  // CHECK: [2] StackObject(21) destructed from level 2
  // CHECK: [3] StackObject(1) destructed from level 1
  // CHECK: [4] StackObject(0) destructed from level 0
  // CHECK: main: caught 42

  // CHECK: total objects destructed during unwind = 5
  printf("total objects destructed during unwind = %d\n", g_dtor_order);

  // CHECK: PASS
  printf("PASS\n");
  return 0;
}
