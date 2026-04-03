// Test that thread_local destructors run BEFORE static destructors per Itanium ABI.
//
// RUN: %clang_crt_main -std=c++17 %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s
//
// REQUIRES: windows, crt

#include <stdio.h>

// Track destruction order.
static int g_order = 0;

struct StaticObject {
  int id;
  StaticObject(int i) : id(i) {
    printf("StaticObject(%d) constructed\n", id);
  }
  ~StaticObject() {
    printf("StaticObject(%d) destructed at order %d\n", id, g_order++);
  }
};

struct ThreadLocalObject {
  int id;
  ThreadLocalObject(int i) : id(i) {
    printf("ThreadLocalObject(%d) constructed\n", id);
  }
  ~ThreadLocalObject() {
    printf("ThreadLocalObject(%d) destructed at order %d\n", id, g_order++);
  }
};

// Construct statics first.
static StaticObject static1(1);
static StaticObject static2(2);

// Thread-locals constructed on first access.
thread_local ThreadLocalObject tls1(10);
thread_local ThreadLocalObject tls2(20);

int main() {
  // CHECK: Thread-local vs static ordering test
  printf("Thread-local vs static ordering test\n");

  // CHECK: StaticObject(1) constructed
  // CHECK: StaticObject(2) constructed

  // Access thread-locals to construct them.
  // CHECK: ThreadLocalObject(10) constructed
  printf("tls1.id = %d\n", tls1.id);

  // CHECK: ThreadLocalObject(20) constructed
  printf("tls2.id = %d\n", tls2.id);

  // CHECK: main done
  printf("main done\n");
  return 0;
}

// Per Itanium ABI [basic.start.term]/1:
// Thread-local destructors run before static destructors.
// Within each category, LIFO order.

// Thread-locals destruct first (LIFO within):
// CHECK: ThreadLocalObject(20) destructed at order 0
// CHECK: ThreadLocalObject(10) destructed at order 1

// Then statics (LIFO within):
// CHECK: StaticObject(2) destructed at order 2
// CHECK: StaticObject(1) destructed at order 3
